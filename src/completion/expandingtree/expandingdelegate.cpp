/*
    SPDX-FileCopyrightText: 2006 Hamish Rodda <rodda@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "expandingdelegate.h"

#include <QApplication>
#include <QBrush>
#include <QKeyEvent>
#include <QPainter>
#include <QTextLine>
#include <QTreeView>

#include "katepartdebug.h"

#include "expandingwidgetmodel.h"

ExpandingDelegate::ExpandingDelegate(ExpandingWidgetModel *model, QObject *parent)
    : QItemDelegate(parent)
    , m_currentColumnStart(0)
    , m_model(model)
{
}

// Gets the background-color in the way QItemDelegate does it
static QColor getUsedBackgroundColor(const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (option.showDecorationSelected && (option.state & QStyle::State_Selected)) {
        QPalette::ColorGroup cg = (option.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
        if (cg == QPalette::Normal && !(option.state & QStyle::State_Active)) {
            cg = QPalette::Inactive;
        }

        return option.palette.brush(cg, QPalette::Highlight).color();
    } else {
        QVariant value = index.data(Qt::BackgroundRole);
        if (value.canConvert<QBrush>()) {
            return qvariant_cast<QBrush>(value).color();
        }
    }

    return QApplication::palette().base().color();
}

static void dampColors(QColor &col)
{
    // Reduce the colors that are less visible to the eye, because they are closer to black when it comes to contrast
    // The most significant color to the eye is green. Then comes red, and then blue, with blue _much_ less significant.

    col.setBlue(0);
    col.setRed(col.red() / 2);
}

// A hack to compute more eye-focused contrast values
static double readabilityContrast(QColor foreground, QColor background)
{
    dampColors(foreground);
    dampColors(background);
    return abs(foreground.green() - background.green()) + abs(foreground.red() - background.red()) + abs(foreground.blue() - background.blue());
}

void ExpandingDelegate::paint(QPainter *painter, const QStyleOptionViewItem &optionOld, const QModelIndex &index) const
{
    QStyleOptionViewItem option(optionOld);

    m_currentIndex = index;

    adjustStyle(index, option);

    if (index.column() == 0) {
        model()->placeExpandingWidget(index);
    }

    // Make sure the decorations are painted at the top, because the center of expanded items will be filled with the embedded widget.
    if (model()->isPartiallyExpanded(index) == ExpandingWidgetModel::ExpandUpwards) {
        m_cachedAlignment = Qt::AlignBottom;
    } else {
        m_cachedAlignment = Qt::AlignTop;
    }

    option.decorationAlignment = m_cachedAlignment;
    option.displayAlignment = m_cachedAlignment;

    // qCDebug(LOG_KTE) << "Painting row " << index.row() << ", column " << index.column() << ", internal " << index.internalPointer() << ", drawselected " <<
    // option.showDecorationSelected << ", selected " << (option.state & QStyle::State_Selected);

    m_cachedHighlights.clear();
    m_backgroundColor = getUsedBackgroundColor(option, index);

    if (model()->indexIsItem(index)) {
        m_currentColumnStart = 0;
        m_cachedHighlights = createHighlighting(index, option);
    }

    /*qCDebug(LOG_KTE) << "Highlights for line:";
    for (const QTextLayout::FormatRange& fr : m_cachedHighlights)
      qCDebug(LOG_KTE) << fr.start << " len " << fr.length << " format ";*/

    QItemDelegate::paint(painter, option, index);

    /// This is a bug workaround for the Qt raster paint engine: It paints over widgets embedded into the viewport when updating due to mouse events
    ///@todo report to Qt Software
    if (model()->isExpanded(index) && model()->expandingWidget(index)) {
        model()->expandingWidget(index)->update();
    }
}

QVector<QTextLayout::FormatRange> ExpandingDelegate::createHighlighting(const QModelIndex &index, QStyleOptionViewItem &option) const
{
    Q_UNUSED(index);
    Q_UNUSED(option);
    return QVector<QTextLayout::FormatRange>();
}

QSize ExpandingDelegate::basicSizeHint(const QModelIndex &index) const
{
    return QItemDelegate::sizeHint(QStyleOptionViewItem(), index);
}

QSize ExpandingDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize s = QItemDelegate::sizeHint(option, index);
    if (model()->isExpanded(index) && model()->expandingWidget(index)) {
        QWidget *widget = model()->expandingWidget(index);
        QSize widgetSize = widget->size();

        s.setHeight(widgetSize.height() + s.height()
                    + 10); // 10 is the sum that must match exactly the offsets used in ExpandingWidgetModel::placeExpandingWidgets
    } else if (model()->isPartiallyExpanded(index)) {
        s.setHeight(s.height() + 30 + 10);
    }
    return s;
}

void ExpandingDelegate::adjustStyle(const QModelIndex &index, QStyleOptionViewItem &option) const
{
    Q_UNUSED(index)
    Q_UNUSED(option)
}

void ExpandingDelegate::adjustRect(QRect &rect) const
{
    if (!model()->indexIsItem(m_currentIndex) /*&& m_currentIndex.column() == 0*/) {
        rect.setLeft(model()->treeView()->columnViewportPosition(0));
        int columnCount = model()->columnCount(m_currentIndex.parent());

        if (!columnCount) {
            return;
        }
        rect.setRight(model()->treeView()->columnViewportPosition(columnCount - 1) + model()->treeView()->columnWidth(columnCount - 1));
    }
}

void ExpandingDelegate::drawDisplay(QPainter *painter, const QStyleOptionViewItem &option, const QRect &_rect, const QString &text) const
{
    QRect rect(_rect);

    adjustRect(rect);

    QTextLayout layout(text, option.font, painter->device());

    QVector<QTextLayout::FormatRange> additionalFormats;

    int missingFormats = text.length();

    for (int i = 0; i < m_cachedHighlights.count(); ++i) {
        if (m_cachedHighlights[i].start + m_cachedHighlights[i].length <= m_currentColumnStart) {
            continue;
        }

        if (additionalFormats.isEmpty()) {
            if (i != 0 && m_cachedHighlights[i - 1].start + m_cachedHighlights[i - 1].length > m_currentColumnStart) {
                QTextLayout::FormatRange before;
                before.start = 0;
                before.length = m_cachedHighlights[i - 1].start + m_cachedHighlights[i - 1].length - m_currentColumnStart;
                before.format = m_cachedHighlights[i - 1].format;
                additionalFormats.append(before);
            }
        }

        QTextLayout::FormatRange format;
        format.start = m_cachedHighlights[i].start - m_currentColumnStart;
        format.length = m_cachedHighlights[i].length;
        format.format = m_cachedHighlights[i].format;

        additionalFormats.append(format);
    }
    if (!additionalFormats.isEmpty()) {
        missingFormats = text.length() - (additionalFormats.back().length + additionalFormats.back().start);
    }

    if (missingFormats > 0) {
        QTextLayout::FormatRange format;
        format.start = text.length() - missingFormats;
        format.length = missingFormats;
        QTextCharFormat fm;
        fm.setForeground(option.palette.text());
        format.format = fm;
        additionalFormats.append(format);
    }

    if (m_backgroundColor.isValid()) {
        QColor background = m_backgroundColor;
        //     qCDebug(LOG_KTE) << text << "background:" << background.name();
        // Now go through the formats, and make sure the contrast background/foreground is readable
        for (int a = 0; a < additionalFormats.size(); ++a) {
            QColor currentBackground = background;
            if (additionalFormats[a].format.hasProperty(QTextFormat::BackgroundBrush)) {
                currentBackground = additionalFormats[a].format.background().color();
            }

            QColor currentColor = additionalFormats[a].format.foreground().color();

            double currentContrast = readabilityContrast(currentColor, currentBackground);
            QColor invertedColor(0xffffffff - additionalFormats[a].format.foreground().color().rgb());
            double invertedContrast = readabilityContrast(invertedColor, currentBackground);

            //       qCDebug(LOG_KTE) << "values:" << invertedContrast << currentContrast << invertedColor.name() << currentColor.name();

            if (invertedContrast > currentContrast) {
                //         qCDebug(LOG_KTE) << text << additionalFormats[a].length << "switching from" << currentColor.name() << "to" << invertedColor.name();
                QBrush b(additionalFormats[a].format.foreground());
                b.setColor(invertedColor);
                additionalFormats[a].format.setForeground(b);
            }
        }
    }

    for (int a = additionalFormats.size() - 1; a >= 0; --a) {
        if (additionalFormats[a].length == 0) {
            additionalFormats.removeAt(a);
        } else {
            /// For some reason the text-formats seem to be invalid in some way, sometimes
            ///@todo Fix this properly, it sucks not copying everything over
            QTextCharFormat fm;
            fm.setForeground(QBrush(additionalFormats[a].format.foreground().color()));
            fm.setBackground(additionalFormats[a].format.background());
            fm.setUnderlineStyle(additionalFormats[a].format.underlineStyle());
            fm.setUnderlineColor(additionalFormats[a].format.underlineColor());
            fm.setFontWeight(additionalFormats[a].format.fontWeight());
            additionalFormats[a].format = fm;
        }
    }

    layout.setFormats(additionalFormats);

    QTextOption to;

    to.setAlignment(static_cast<Qt::Alignment>(m_cachedAlignment | option.displayAlignment));

    to.setWrapMode(QTextOption::WrapAnywhere);
    layout.setTextOption(to);

    layout.beginLayout();
    QTextLine line = layout.createLine();
    // Leave some extra space when the text is right-aligned
    line.setLineWidth(rect.width() - (option.displayAlignment == Qt::AlignRight ? 8 : 0));
    layout.endLayout();

    // We need to do some hand layouting here
    if (to.alignment() & Qt::AlignBottom) {
        layout.draw(painter, QPoint(rect.left(), rect.bottom() - (int)line.height()));
    } else {
        layout.draw(painter, rect.topLeft());
    }

    return;

    // if (painter->fontMetrics().width(text) > textRect.width() && !text.contains(QLatin1Char('\n')))
    // str = elidedText(option.fontMetrics, textRect.width(), option.textElideMode, text);
    // qt_format_text(option.font, textRect, option.displayAlignment, str, 0, 0, 0, 0, painter);
}

void ExpandingDelegate::drawDecoration(QPainter *painter, const QStyleOptionViewItem &option, const QRect &rect, const QPixmap &pixmap) const
{
    if (model()->indexIsItem(m_currentIndex)) {
        QItemDelegate::drawDecoration(painter, option, rect, pixmap);
    }
}

void ExpandingDelegate::drawBackground(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index)
    // Problem: This isn't called at all, because drawBrackground is not virtual :-/
    QStyle *style = model()->treeView()->style() ? model()->treeView()->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &option, painter);
}

ExpandingWidgetModel *ExpandingDelegate::model() const
{
    return m_model;
}

void ExpandingDelegate::heightChanged() const
{
}

bool ExpandingDelegate::editorEvent(QEvent *event, QAbstractItemModel * /*model*/, const QStyleOptionViewItem & /*option*/, const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        event->accept();
        model()->setExpanded(index, !model()->isExpanded(index));
        heightChanged();

        return true;
    } else {
        event->ignore();
    }

    return false;
}

QVector<QTextLayout::FormatRange> ExpandingDelegate::highlightingFromVariantList(const QList<QVariant> &customHighlights) const
{
    QVector<QTextLayout::FormatRange> ret;

    for (int i = 0; i + 2 < customHighlights.count(); i += 3) {
        if (!customHighlights[i].canConvert(QVariant::Int) || !customHighlights[i + 1].canConvert(QVariant::Int)
            || !customHighlights[i + 2].canConvert<QTextFormat>()) {
            qCWarning(LOG_KTE) << "Unable to convert triple to custom formatting.";
            continue;
        }

        QTextLayout::FormatRange format;
        format.start = customHighlights[i].toInt();
        format.length = customHighlights[i + 1].toInt();
        format.format = customHighlights[i + 2].value<QTextFormat>().toCharFormat();

        if (!format.format.isValid()) {
            qCWarning(LOG_KTE) << "Format is not valid";
        }

        ret << format;
    }
    return ret;
}
