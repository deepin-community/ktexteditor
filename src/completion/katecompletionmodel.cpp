/*
    SPDX-FileCopyrightText: 2005-2006 Hamish Rodda <rodda@kde.org>
    SPDX-FileCopyrightText: 2007-2008 David Nolden <david.nolden.kdevelop@art-master.de>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katecompletionmodel.h"

#include "kateargumenthintmodel.h"
#include "katecompletiondelegate.h"
#include "katecompletiontree.h"
#include "katecompletionwidget.h"
#include "kateconfig.h"
#include "katepartdebug.h"
#include "katerenderer.h"
#include "kateview.h"
#include <ktexteditor/codecompletionmodelcontrollerinterface.h>

#include <KLocalizedString>

#include <QApplication>
#include <QMultiMap>
#include <QTextEdit>
#include <QTimer>
#include <QVarLengthArray>

using namespace KTextEditor;

/// A helper-class for handling completion-models with hierarchical grouping/optimization
class HierarchicalModelHandler
{
public:
    explicit HierarchicalModelHandler(CodeCompletionModel *model);
    void addValue(CodeCompletionModel::ExtraItemDataRoles role, const QVariant &value);
    // Walks the index upwards and collects all defined completion-roles on the way
    void collectRoles(const QModelIndex &index);
    void takeRole(const QModelIndex &index);

    CodeCompletionModel *model() const;

    // Assumes that index is a sub-index of the indices where role-values were taken
    QVariant getData(CodeCompletionModel::ExtraItemDataRoles role, const QModelIndex &index) const;

    bool hasHierarchicalRoles() const;

    int inheritanceDepth(const QModelIndex &i) const;

    QString customGroup() const
    {
        return m_customGroup;
    }

    int customGroupingKey() const
    {
        return m_groupSortingKey;
    }

private:
    typedef QMap<CodeCompletionModel::ExtraItemDataRoles, QVariant> RoleMap;
    RoleMap m_roleValues;
    QString m_customGroup;
    int m_groupSortingKey;
    CodeCompletionModel *m_model;
};

CodeCompletionModel *HierarchicalModelHandler::model() const
{
    return m_model;
}

bool HierarchicalModelHandler::hasHierarchicalRoles() const
{
    return !m_roleValues.isEmpty();
}

void HierarchicalModelHandler::collectRoles(const QModelIndex &index)
{
    if (index.parent().isValid()) {
        collectRoles(index.parent());
    }
    if (m_model->rowCount(index) != 0) {
        takeRole(index);
    }
}

int HierarchicalModelHandler::inheritanceDepth(const QModelIndex &i) const
{
    return getData(CodeCompletionModel::InheritanceDepth, i).toInt();
}

void HierarchicalModelHandler::takeRole(const QModelIndex &index)
{
    QVariant v = index.data(CodeCompletionModel::GroupRole);
    if (v.isValid() && v.canConvert(QVariant::Int)) {
        QVariant value = index.data(v.toInt());
        if (v.toInt() == Qt::DisplayRole) {
            m_customGroup = index.data(Qt::DisplayRole).toString();
            QVariant sortingKey = index.data(CodeCompletionModel::InheritanceDepth);
            if (sortingKey.canConvert(QVariant::Int)) {
                m_groupSortingKey = sortingKey.toInt();
            }
        } else {
            m_roleValues[(CodeCompletionModel::ExtraItemDataRoles)v.toInt()] = value;
        }
    } else {
        qCDebug(LOG_KTE) << "Did not return valid GroupRole in hierarchical completion-model";
    }
}

QVariant HierarchicalModelHandler::getData(CodeCompletionModel::ExtraItemDataRoles role, const QModelIndex &index) const
{
    RoleMap::const_iterator it = m_roleValues.find(role);
    if (it != m_roleValues.end()) {
        return *it;
    } else {
        return index.data(role);
    }
}

HierarchicalModelHandler::HierarchicalModelHandler(CodeCompletionModel *model)
    : m_groupSortingKey(-1)
    , m_model(model)
{
}

void HierarchicalModelHandler::addValue(CodeCompletionModel::ExtraItemDataRoles role, const QVariant &value)
{
    m_roleValues[role] = value;
}

KateCompletionModel::KateCompletionModel(KateCompletionWidget *parent)
    : ExpandingWidgetModel(parent)
    , m_ungrouped(new Group({}, 0, this))
    , m_argumentHints(new Group(i18n("Argument-hints"), -1, this))
    , m_bestMatches(new Group(i18n("Best matches"), BestMatchesProperty, this))
{
    m_emptyGroups.append(m_ungrouped);
    m_emptyGroups.append(m_argumentHints);
    m_emptyGroups.append(m_bestMatches);

    m_updateBestMatchesTimer = new QTimer(this);
    m_updateBestMatchesTimer->setSingleShot(true);
    connect(m_updateBestMatchesTimer, &QTimer::timeout, this, &KateCompletionModel::updateBestMatches);

    m_groupHash.insert(0, m_ungrouped);
    m_groupHash.insert(-1, m_argumentHints);
    m_groupHash.insert(BestMatchesProperty, m_argumentHints);
}

KateCompletionModel::~KateCompletionModel()
{
    clearCompletionModels();
    delete m_argumentHints;
    delete m_ungrouped;
    delete m_bestMatches;
}

QTreeView *KateCompletionModel::treeView() const
{
    return view()->completionWidget()->treeView();
}

QVariant KateCompletionModel::data(const QModelIndex &index, int role) const
{
    if (!hasCompletionModel() || !index.isValid()) {
        return QVariant();
    }

    if (role == Qt::DecorationRole && index.column() == KTextEditor::CodeCompletionModel::Prefix && isExpandable(index)) {
        cacheIcons();

        if (!isExpanded(index)) {
            return QVariant(m_collapsedIcon);
        } else {
            return QVariant(m_expandedIcon);
        }
    }

    // groupOfParent returns a group when the index is a member of that group, but not the group head/label.
    if (!hasGroups() || groupOfParent(index)) {
        if (role == Qt::TextAlignmentRole) {
            if (isColumnMergingEnabled() && !m_columnMerges.isEmpty()) {
                int c = 0;
                for (const QList<int> &list : std::as_const(m_columnMerges)) {
                    if (index.column() < c + list.size()) {
                        c += list.size();
                        continue;
                    } else if (list.count() == 1 && list.first() == CodeCompletionModel::Scope) {
                        return Qt::AlignRight;
                    } else {
                        return QVariant();
                    }
                }

            } else if ((!isColumnMergingEnabled() || m_columnMerges.isEmpty()) && index.column() == CodeCompletionModel::Scope) {
                return Qt::AlignRight;
            }
        }

        // Merge text for column merging
        if (role == Qt::DisplayRole && !m_columnMerges.isEmpty() && isColumnMergingEnabled()) {
            QString text;
            for (int column : m_columnMerges[index.column()]) {
                QModelIndex sourceIndex = mapToSource(createIndex(index.row(), column, index.internalPointer()));
                text.append(sourceIndex.data(role).toString());
            }

            return text;
        }

        if (role == CodeCompletionModel::HighlightingMethod) {
            // Return that we are doing custom-highlighting of one of the sub-strings does it. Unfortunately internal highlighting does not work for the other
            // substrings.
            for (int column : m_columnMerges[index.column()]) {
                QModelIndex sourceIndex = mapToSource(createIndex(index.row(), column, index.internalPointer()));
                QVariant method = sourceIndex.data(CodeCompletionModel::HighlightingMethod);
                if (method.type() == QVariant::Int && method.toInt() == CodeCompletionModel::CustomHighlighting) {
                    return QVariant(CodeCompletionModel::CustomHighlighting);
                }
            }
            return QVariant();
        }
        if (role == CodeCompletionModel::CustomHighlight) {
            // Merge custom highlighting if multiple columns were merged
            QStringList strings;

            // Collect strings
            const auto &columns = m_columnMerges[index.column()];
            strings.reserve(columns.size());
            for (int column : columns) {
                strings << mapToSource(createIndex(index.row(), column, index.internalPointer())).data(Qt::DisplayRole).toString();
            }

            QList<QVariantList> highlights;

            // Collect custom-highlightings
            highlights.reserve(columns.size());
            for (int column : columns) {
                highlights << mapToSource(createIndex(index.row(), column, index.internalPointer())).data(CodeCompletionModel::CustomHighlight).toList();
            }

            return mergeCustomHighlighting(strings, highlights, 0);
        }

        QVariant v = mapToSource(index).data(role);
        if (v.isValid()) {
            return v;
        } else {
            return ExpandingWidgetModel::data(index, role);
        }
    }

    // Returns a nonzero group if this index is the head of a group(A Label in the list)
    Group *g = groupForIndex(index);

    if (g && (!g->isEmpty)) {
        switch (role) {
        case Qt::DisplayRole:
            if (!index.column()) {
                return g->title;
            }
            break;

        case Qt::FontRole:
            if (!index.column()) {
                QFont f = view()->renderer()->currentFont();
                f.setBold(true);
                return f;
            }
            break;

        case Qt::ForegroundRole:
            return QApplication::palette().toolTipText().color();
        case Qt::BackgroundRole:
            return QApplication::palette().toolTipBase().color();
        }
    }

    return QVariant();
}

int KateCompletionModel::contextMatchQuality(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return 0;
    }
    Group *g = groupOfParent(index);
    if (!g || g->filtered.size() < (size_t)index.row()) {
        return 0;
    }

    return contextMatchQuality(g->filtered[index.row()].sourceRow());
}

int KateCompletionModel::contextMatchQuality(const ModelRow &source) const
{
    QModelIndex realIndex = source.second;

    int bestMatch = -1;
    // Iterate through all argument-hints and find the best match-quality
    for (const Item &item : std::as_const(m_argumentHints->filtered)) {
        const ModelRow &row(item.sourceRow());
        if (realIndex.model() != row.first) {
            continue; // We can only match within the same source-model
        }

        QModelIndex hintIndex = row.second;

        QVariant depth = hintIndex.data(CodeCompletionModel::ArgumentHintDepth);
        if (!depth.isValid() || depth.type() != QVariant::Int || depth.toInt() != 1) {
            continue; // Only match completion-items to argument-hints of depth 1(the ones the item will be given to as argument)
        }

        hintIndex.data(CodeCompletionModel::SetMatchContext);

        QVariant matchQuality = realIndex.data(CodeCompletionModel::MatchQuality);
        if (matchQuality.isValid() && matchQuality.type() == QVariant::Int) {
            int m = matchQuality.toInt();
            if (m > bestMatch) {
                bestMatch = m;
            }
        }
    }

    if (m_argumentHints->filtered.empty()) {
        QVariant matchQuality = realIndex.data(CodeCompletionModel::MatchQuality);
        if (matchQuality.isValid() && matchQuality.type() == QVariant::Int) {
            int m = matchQuality.toInt();
            if (m > bestMatch) {
                bestMatch = m;
            }
        }
    }

    return bestMatch;
}

Qt::ItemFlags KateCompletionModel::flags(const QModelIndex &index) const
{
    if (!hasCompletionModel() || !index.isValid()) {
        return Qt::NoItemFlags;
    }

    if (!hasGroups() || groupOfParent(index)) {
        return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    }

    return Qt::ItemIsEnabled;
}

KateCompletionWidget *KateCompletionModel::widget() const
{
    return static_cast<KateCompletionWidget *>(QObject::parent());
}

KTextEditor::ViewPrivate *KateCompletionModel::view() const
{
    return widget()->view();
}

int KateCompletionModel::columnCount(const QModelIndex &) const
{
    return isColumnMergingEnabled() && !m_columnMerges.isEmpty() ? m_columnMerges.count() : KTextEditor::CodeCompletionModel::ColumnCount;
}

KateCompletionModel::ModelRow KateCompletionModel::modelRowPair(const QModelIndex &index) const
{
    return qMakePair(static_cast<CodeCompletionModel *>(const_cast<QAbstractItemModel *>(index.model())), index);
}

bool KateCompletionModel::hasChildren(const QModelIndex &parent) const
{
    if (!hasCompletionModel()) {
        return false;
    }

    if (!parent.isValid()) {
        if (hasGroups()) {
            return true;
        }

        return !m_ungrouped->filtered.empty();
    }

    if (parent.column() != 0) {
        return false;
    }

    if (!hasGroups()) {
        return false;
    }

    if (Group *g = groupForIndex(parent)) {
        return !g->filtered.empty();
    }

    return false;
}

QModelIndex KateCompletionModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= columnCount(QModelIndex())) {
        return QModelIndex();
    }

    if (parent.isValid() || !hasGroups()) {
        if (parent.isValid() && parent.column() != 0) {
            return QModelIndex();
        }

        Group *g = groupForIndex(parent);

        if (!g) {
            return QModelIndex();
        }

        if (row >= (int)g->filtered.size()) {
            // qCWarning(LOG_KTE) << "Invalid index requested: row " << row << " beyond individual range in group " << g;
            return QModelIndex();
        }

        // qCDebug(LOG_KTE) << "Returning index for child " << row << " of group " << g;
        return createIndex(row, column, g);
    }

    if (row >= m_rowTable.count()) {
        // qCWarning(LOG_KTE) << "Invalid index requested: row " << row << " beyond group range.";
        return QModelIndex();
    }

    // qCDebug(LOG_KTE) << "Returning index for group " << m_rowTable[row];
    return createIndex(row, column, quintptr(0));
}

/*QModelIndex KateCompletionModel::sibling( int row, int column, const QModelIndex & index ) const
{
  if (row < 0 || column < 0 || column >= columnCount(QModelIndex()))
    return QModelIndex();

  if (!index.isValid()) {
  }

  if (Group* g = groupOfParent(index)) {
    if (row >= g->filtered.count())
      return QModelIndex();

    return createIndex(row, column, g);
  }

  if (hasGroups())
    return QModelIndex();

  if (row >= m_ungrouped->filtered.count())
    return QModelIndex();

  return createIndex(row, column, m_ungrouped);
}*/

bool KateCompletionModel::hasIndex(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= columnCount(QModelIndex())) {
        return false;
    }

    if (parent.isValid() || !hasGroups()) {
        if (parent.isValid() && parent.column() != 0) {
            return false;
        }

        Group *g = groupForIndex(parent);

        if (row >= (int)g->filtered.size()) {
            return false;
        }

        return true;
    }

    if (row >= m_rowTable.count()) {
        return false;
    }

    return true;
}

QModelIndex KateCompletionModel::indexForRow(Group *g, int row) const
{
    if (row < 0 || row >= (int)g->filtered.size()) {
        return QModelIndex();
    }

    return createIndex(row, 0, g);
}

QModelIndex KateCompletionModel::indexForGroup(Group *g) const
{
    if (!hasGroups()) {
        return QModelIndex();
    }

    int row = m_rowTable.indexOf(g);

    if (row == -1) {
        return QModelIndex();
    }

    return createIndex(row, 0, quintptr(0));
}

void KateCompletionModel::clearGroups()
{
    clearExpanding();
    m_ungrouped->clear();
    m_argumentHints->clear();
    m_bestMatches->clear();

    // Don't bother trying to work out where it is
    m_rowTable.removeAll(m_ungrouped);
    m_emptyGroups.removeAll(m_ungrouped);

    m_rowTable.removeAll(m_argumentHints);
    m_emptyGroups.removeAll(m_argumentHints);

    m_rowTable.removeAll(m_bestMatches);
    m_emptyGroups.removeAll(m_bestMatches);

    qDeleteAll(m_rowTable);
    qDeleteAll(m_emptyGroups);
    m_rowTable.clear();
    m_emptyGroups.clear();
    m_groupHash.clear();
    m_customGroupHash.clear();

    m_emptyGroups.append(m_ungrouped);
    m_groupHash.insert(0, m_ungrouped);

    m_emptyGroups.append(m_argumentHints);
    m_groupHash.insert(-1, m_argumentHints);

    m_emptyGroups.append(m_bestMatches);
    m_groupHash.insert(BestMatchesProperty, m_bestMatches);
}

QSet<KateCompletionModel::Group *> KateCompletionModel::createItems(const HierarchicalModelHandler &_handler, const QModelIndex &i, bool notifyModel)
{
    HierarchicalModelHandler handler(_handler);
    QSet<Group *> ret;
    QAbstractItemModel *model = handler.model();

    if (model->rowCount(i) == 0) {
        // Leaf node, create an item
        ret.insert(createItem(handler, i, notifyModel));
    } else {
        // Non-leaf node, take the role from the node, and recurse to the sub-nodes
        handler.takeRole(i);
        for (int a = 0; a < model->rowCount(i); a++) {
            ret += createItems(handler, model->index(a, 0, i), notifyModel);
        }
    }

    return ret;
}

QSet<KateCompletionModel::Group *> KateCompletionModel::deleteItems(const QModelIndex &i)
{
    QSet<Group *> ret;

    if (i.model()->rowCount(i) == 0) {
        // Leaf node, delete the item
        Group *g = groupForIndex(mapFromSource(i));
        ret.insert(g);
        g->removeItem(ModelRow(const_cast<CodeCompletionModel *>(static_cast<const CodeCompletionModel *>(i.model())), i));
    } else {
        // Non-leaf node
        for (int a = 0; a < i.model()->rowCount(i); a++) {
            ret += deleteItems(i.model()->index(a, 0, i));
        }
    }

    return ret;
}

void KateCompletionModel::createGroups()
{
    beginResetModel();
    // After clearing the model, it has to be reset, else we will be in an invalid state while inserting
    // new groups.
    clearGroups();

    QSet<Group *> groups;

    bool has_groups = false;
    for (CodeCompletionModel *sourceModel : std::as_const(m_completionModels)) {
        has_groups |= sourceModel->hasGroups();
        for (int i = 0; i < sourceModel->rowCount(); ++i) {
            groups += createItems(HierarchicalModelHandler(sourceModel), sourceModel->index(i, 0));
        }
    }

    // since notifyModel = false above, we just appended the data as is,
    // we sort it now
    for (auto g : groups) {
        std::sort(g->prefilter.begin(), g->prefilter.end());
        std::sort(g->filtered.begin(), g->filtered.end());
    }

    m_hasGroups = has_groups;

    // debugStats();

    for (Group *g : std::as_const(m_rowTable)) {
        hideOrShowGroup(g);
    }

    for (Group *g : std::as_const(m_emptyGroups)) {
        hideOrShowGroup(g);
    }

    makeGroupItemsUnique();

    updateBestMatches();
    endResetModel();
}

KateCompletionModel::Group *KateCompletionModel::createItem(const HierarchicalModelHandler &handler, const QModelIndex &sourceIndex, bool notifyModel)
{
    // QModelIndex sourceIndex = sourceModel->index(row, CodeCompletionModel::Name, QModelIndex());

    int completionFlags = handler.getData(CodeCompletionModel::CompletionRole, sourceIndex).toInt();

    // Scope is expensive, should not be used with big models
    QString scopeIfNeeded =
        (groupingMethod() & Scope) ? sourceIndex.sibling(sourceIndex.row(), CodeCompletionModel::Scope).data(Qt::DisplayRole).toString() : QString();

    int argumentHintDepth = handler.getData(CodeCompletionModel::ArgumentHintDepth, sourceIndex).toInt();

    Group *g;
    if (argumentHintDepth) {
        g = m_argumentHints;
    } else {
        QString customGroup = handler.customGroup();
        if (!customGroup.isNull() && m_hasGroups) {
            if (m_customGroupHash.contains(customGroup)) {
                g = m_customGroupHash[customGroup];
            } else {
                g = new Group(customGroup, 0, this);
                g->customSortingKey = handler.customGroupingKey();
                m_emptyGroups.append(g);
                m_customGroupHash.insert(customGroup, g);
            }
        } else {
            g = fetchGroup(completionFlags, scopeIfNeeded, handler.hasHierarchicalRoles());
        }
    }

    Item item = Item(g != m_argumentHints, this, handler, ModelRow(handler.model(), sourceIndex));

    if (g != m_argumentHints) {
        item.match();
    }

    g->addItem(item, notifyModel);

    return g;
}

void KateCompletionModel::slotRowsInserted(const QModelIndex &parent, int start, int end)
{

    HierarchicalModelHandler handler(static_cast<CodeCompletionModel *>(sender()));
    if (parent.isValid()) {
        handler.collectRoles(parent);
    }

    QSet<Group *> affectedGroups;
    for (int i = start; i <= end; ++i) {
        affectedGroups += createItems(handler, handler.model()->index(i, 0, parent), /* notifyModel= */ true);
    }

    for (Group *g : std::as_const(affectedGroups)) {
        hideOrShowGroup(g, true);
    }
}

void KateCompletionModel::slotRowsRemoved(const QModelIndex &parent, int start, int end)
{
    CodeCompletionModel *source = static_cast<CodeCompletionModel *>(sender());

    QSet<Group *> affectedGroups;

    for (int i = start; i <= end; ++i) {
        QModelIndex index = source->index(i, 0, parent);

        affectedGroups += deleteItems(index);
    }

    for (Group *g : std::as_const(affectedGroups)) {
        hideOrShowGroup(g, true);
    }
}

KateCompletionModel::Group *KateCompletionModel::fetchGroup(int attribute, const QString &scope, bool forceGrouping)
{
    Q_UNUSED(forceGrouping);

    ///@todo use forceGrouping
    if (!hasGroups()) {
        return m_ungrouped;
    }

    int groupingAttribute = groupingAttributes(attribute);
    // qCDebug(LOG_KTE) << attribute << " " << groupingAttribute;

    if (m_groupHash.contains(groupingAttribute)) {
        if (groupingMethod() & Scope) {
            for (QHash<int, Group *>::ConstIterator it = m_groupHash.constFind(groupingAttribute);
                 it != m_groupHash.constEnd() && it.key() == groupingAttribute;
                 ++it) {
                if (it.value()->scope == scope) {
                    return it.value();
                }
            }
        } else {
            return m_groupHash.value(groupingAttribute);
        }
    }

    QString st;
    QString at;
    QString it;
    QString title;

    if (groupingMethod() & ScopeType) {
        if (attribute & KTextEditor::CodeCompletionModel::GlobalScope) {
            st = QStringLiteral("Global");
        } else if (attribute & KTextEditor::CodeCompletionModel::NamespaceScope) {
            st = QStringLiteral("Namespace");
        } else if (attribute & KTextEditor::CodeCompletionModel::LocalScope) {
            st = QStringLiteral("Local");
        }

        title = st;
    }

    if (groupingMethod() & Scope) {
        if (!title.isEmpty()) {
            title.append(QLatin1Char(' '));
        }

        title.append(scope);
    }

    if (groupingMethod() & AccessType) {
        if (attribute & KTextEditor::CodeCompletionModel::Public) {
            at = QStringLiteral("Public");
        } else if (attribute & KTextEditor::CodeCompletionModel::Protected) {
            at = QStringLiteral("Protected");
        } else if (attribute & KTextEditor::CodeCompletionModel::Private) {
            at = QStringLiteral("Private");
        }

        if (accessIncludeStatic() && attribute & KTextEditor::CodeCompletionModel::Static) {
            at.append(QLatin1String(" Static"));
        }

        if (accessIncludeConst() && attribute & KTextEditor::CodeCompletionModel::Const) {
            at.append(QLatin1String(" Const"));
        }

        if (!at.isEmpty()) {
            if (!title.isEmpty()) {
                title.append(QLatin1String(", "));
            }

            title.append(at);
        }
    }

    if (groupingMethod() & ItemType) {
        if (attribute & CodeCompletionModel::Namespace) {
            it = i18n("Namespaces");
        } else if (attribute & CodeCompletionModel::Class) {
            it = i18n("Classes");
        } else if (attribute & CodeCompletionModel::Struct) {
            it = i18n("Structs");
        } else if (attribute & CodeCompletionModel::Union) {
            it = i18n("Unions");
        } else if (attribute & CodeCompletionModel::Function) {
            it = i18n("Functions");
        } else if (attribute & CodeCompletionModel::Variable) {
            it = i18n("Variables");
        } else if (attribute & CodeCompletionModel::Enum) {
            it = i18n("Enumerations");
        }

        if (!it.isEmpty()) {
            if (!title.isEmpty()) {
                title.append(QLatin1Char(' '));
            }

            title.append(it);
        }
    }

    Group *ret = new Group(title, attribute, this);
    ret->scope = scope;

    m_emptyGroups.append(ret);
    m_groupHash.insert(groupingAttribute, ret);

    return ret;
}

bool KateCompletionModel::hasGroups() const
{
    // qCDebug(LOG_KTE) << "m_groupHash.size()"<<m_groupHash.size();
    // qCDebug(LOG_KTE) << "m_rowTable.count()"<<m_rowTable.count();
    // We cannot decide whether there is groups easily. The problem: The code-model can
    // be populated with a delay from within a background-thread.
    // Proper solution: Ask all attached code-models(Through a new interface) whether they want to use grouping,
    // and if at least one wants to, return true, else return false.
    return m_groupingEnabled && m_hasGroups;
}

KateCompletionModel::Group *KateCompletionModel::groupForIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        if (!hasGroups()) {
            return m_ungrouped;
        } else {
            return nullptr;
        }
    }

    if (groupOfParent(index)) {
        return nullptr;
    }

    if (index.row() < 0 || index.row() >= m_rowTable.count()) {
        return m_ungrouped;
    }

    return m_rowTable[index.row()];
}

/*QMap< int, QVariant > KateCompletionModel::itemData( const QModelIndex & index ) const
{
  if (!hasGroups() || groupOfParent(index)) {
    QModelIndex index = mapToSource(index);
    if (index.isValid())
      return index.model()->itemData(index);
  }

  return QAbstractItemModel::itemData(index);
}*/

QModelIndex KateCompletionModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    if (Group *g = groupOfParent(index)) {
        if (!hasGroups()) {
            Q_ASSERT(g == m_ungrouped);
            return QModelIndex();
        }

        int row = m_rowTable.indexOf(g);

        if (row == -1) {
            qCWarning(LOG_KTE) << "Couldn't find parent for index" << index;
            return QModelIndex();
        }

        return createIndex(row, 0, quintptr(0));
    }

    return QModelIndex();
}

int KateCompletionModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        if (hasGroups()) {
            // qCDebug(LOG_KTE) << "Returning row count for toplevel " << m_rowTable.count();
            return m_rowTable.count();
        } else {
            // qCDebug(LOG_KTE) << "Returning ungrouped row count for toplevel " << m_ungrouped->filtered.count();
            return m_ungrouped->filtered.size();
        }
    }

    if (parent.column() > 0) {
        // only the first column has children
        return 0;
    }

    Group *g = groupForIndex(parent);

    // This is not an error, seems you don't have to check hasChildren()
    if (!g) {
        return 0;
    }

    // qCDebug(LOG_KTE) << "Returning row count for group " << g << " as " << g->filtered.count();
    return g->filtered.size();
}

void KateCompletionModel::sort(int column, Qt::SortOrder order)
{
    Q_UNUSED(column)
    Q_UNUSED(order)
}

QModelIndex KateCompletionModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid()) {
        return QModelIndex();
    }

    if (Group *g = groupOfParent(proxyIndex)) {
        if (proxyIndex.row() >= 0 && proxyIndex.row() < (int)g->filtered.size()) {
            ModelRow source = g->filtered[proxyIndex.row()].sourceRow();
            return source.second.sibling(source.second.row(), proxyIndex.column());
        } else {
            qCDebug(LOG_KTE) << "Invalid proxy-index";
        }
    }

    return QModelIndex();
}

QModelIndex KateCompletionModel::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (!sourceIndex.isValid()) {
        return QModelIndex();
    }

    if (!hasGroups()) {
        return index(m_ungrouped->rowOf(modelRowPair(sourceIndex)), sourceIndex.column(), QModelIndex());
    }

    for (Group *g : std::as_const(m_rowTable)) {
        int row = g->rowOf(modelRowPair(sourceIndex));
        if (row != -1) {
            return index(row, sourceIndex.column(), indexForGroup(g));
        }
    }

    // Copied from above
    for (Group *g : std::as_const(m_emptyGroups)) {
        int row = g->rowOf(modelRowPair(sourceIndex));
        if (row != -1) {
            return index(row, sourceIndex.column(), indexForGroup(g));
        }
    }

    return QModelIndex();
}

void KateCompletionModel::setCurrentCompletion(KTextEditor::CodeCompletionModel *model, const QString &completion)
{
    auto &current = m_currentMatch[model];
    if (current == completion) {
        return;
    }

    if (!hasCompletionModel()) {
        current = completion;
        return;
    }

    changeTypes changeType = Change;

    if (current.length() > completion.length() && current.startsWith(completion, m_matchCaseSensitivity)) {
        // Filter has been broadened
        changeType = Broaden;

    } else if (current.length() < completion.length() && completion.startsWith(current, m_matchCaseSensitivity)) {
        // Filter has been narrowed
        changeType = Narrow;
    }

    // qCDebug(LOG_KTE) << model << "Old match: " << current << ", new: " << completion << ", type: " << changeType;

    current = completion;

    const bool resetModel = (changeType != Narrow);
    if (resetModel) {
        beginResetModel();
    }

    if (!hasGroups()) {
        changeCompletions(m_ungrouped, changeType, !resetModel);
    } else {
        for (Group *g : std::as_const(m_rowTable)) {
            if (g != m_argumentHints) {
                changeCompletions(g, changeType, !resetModel);
            }
        }
        for (Group *g : std::as_const(m_emptyGroups)) {
            if (g != m_argumentHints) {
                changeCompletions(g, changeType, !resetModel);
            }
        }
    }

    // NOTE: best matches are also updated in resort
    resort();

    if (resetModel) {
        endResetModel();
    }

    clearExpanding(); // We need to do this, or be aware of expanding-widgets while filtering.

    Q_EMIT layoutChanged();
}

QString KateCompletionModel::commonPrefixInternal(const QString &forcePrefix) const
{
    QString commonPrefix; // isNull() = true

    QList<Group *> groups = m_rowTable;
    groups += m_ungrouped;

    for (Group *g : std::as_const(groups)) {
        for (const Item &item : std::as_const(g->filtered)) {
            uint startPos = m_currentMatch[item.sourceRow().first].length();
            const QString candidate = item.name().mid(startPos);

            if (!candidate.startsWith(forcePrefix)) {
                continue;
            }

            if (commonPrefix.isNull()) {
                commonPrefix = candidate;

                // Replace QString() prefix with QString(), so we won't initialize it again
                if (commonPrefix.isNull()) {
                    commonPrefix = QString(); // isEmpty() = true, isNull() = false
                }
            } else {
                commonPrefix.truncate(candidate.length());

                for (int a = 0; a < commonPrefix.length(); ++a) {
                    if (commonPrefix[a] != candidate[a]) {
                        commonPrefix.truncate(a);
                        break;
                    }
                }
            }
        }
    }

    return commonPrefix;
}

QString KateCompletionModel::commonPrefix(QModelIndex selectedIndex) const
{
    QString commonPrefix = commonPrefixInternal(QString());

    if (commonPrefix.isEmpty() && selectedIndex.isValid()) {
        Group *g = m_ungrouped;
        if (hasGroups()) {
            g = groupOfParent(selectedIndex);
        }

        if (g && selectedIndex.row() < (int)g->filtered.size()) {
            // Follow the path of the selected item, finding the next non-empty common prefix
            Item item = g->filtered[selectedIndex.row()];
            int matchLength = m_currentMatch[item.sourceRow().first].length();
            commonPrefix = commonPrefixInternal(item.name().mid(matchLength).left(1));
        }
    }

    return commonPrefix;
}

void KateCompletionModel::changeCompletions(Group *g, changeTypes changeType, bool notifyModel)
{
    if (changeType != Narrow) {
        g->filtered = g->prefilter;
        // In the "Broaden" or "Change" case, just re-filter everything,
        // and don't notify the model. The model is notified afterwards through a reset().
    }

    // This code determines what of the filtered items still fit, and computes the ranges that were removed, giving
    // them to beginRemoveRows(..) in batches

    std::vector<KateCompletionModel::Item> newFiltered;
    int deleteUntil = -1; // In each state, the range [currentRow+1, deleteUntil] needs to be deleted
    auto &filtered = g->filtered;
    newFiltered.reserve(filtered.size());
    for (int currentRow = filtered.size() - 1; currentRow >= 0; --currentRow) {
        if (filtered[currentRow].match()) {
            // This row does not need to be deleted, which means that currentRow+1 to deleteUntil need to be deleted now
            if (deleteUntil != -1 && notifyModel) {
                beginRemoveRows(indexForGroup(g), currentRow + 1, deleteUntil);
                endRemoveRows();
            }
            deleteUntil = -1;

            newFiltered.push_back(std::move(filtered[currentRow]));
        } else {
            if (deleteUntil == -1) {
                deleteUntil = currentRow; // Mark that this row needs to be deleted
            }
        }
    }

    if (deleteUntil != -1 && notifyModel) {
        beginRemoveRows(indexForGroup(g), 0, deleteUntil);
        endRemoveRows();
    }

    std::reverse(newFiltered.begin(), newFiltered.end());
    g->filtered = std::move(newFiltered);
    hideOrShowGroup(g, notifyModel);
}

int KateCompletionModel::Group::orderNumber() const
{
    if (this == model->m_ungrouped) {
        return 700;
    }

    if (customSortingKey != -1) {
        return customSortingKey;
    }

    if (attribute & BestMatchesProperty) {
        return 1;
    }

    if (attribute & KTextEditor::CodeCompletionModel::LocalScope) {
        return 100;
    } else if (attribute & KTextEditor::CodeCompletionModel::Public) {
        return 200;
    } else if (attribute & KTextEditor::CodeCompletionModel::Protected) {
        return 300;
    } else if (attribute & KTextEditor::CodeCompletionModel::Private) {
        return 400;
    } else if (attribute & KTextEditor::CodeCompletionModel::NamespaceScope) {
        return 500;
    } else if (attribute & KTextEditor::CodeCompletionModel::GlobalScope) {
        return 600;
    }

    return 700;
}

bool KateCompletionModel::Group::orderBefore(Group *other) const
{
    return orderNumber() < other->orderNumber();
}

void KateCompletionModel::hideOrShowGroup(Group *g, bool notifyModel)
{
    if (g == m_argumentHints) {
        Q_EMIT argumentHintsChanged();
        m_updateBestMatchesTimer->start(200); // We have new argument-hints, so we have new best matches
        return; // Never show argument-hints in the normal completion-list
    }

    if (!g->isEmpty) {
        if (g->filtered.empty()) {
            // Move to empty group list
            g->isEmpty = true;
            int row = m_rowTable.indexOf(g);
            if (row != -1) {
                if (hasGroups() && notifyModel) {
                    beginRemoveRows(QModelIndex(), row, row);
                }
                m_rowTable.removeAt(row);
                if (hasGroups() && notifyModel) {
                    endRemoveRows();
                }
                m_emptyGroups.append(g);
            } else {
                qCWarning(LOG_KTE) << "Group " << g << " not found in row table!!";
            }
        }

    } else {
        if (!g->filtered.empty()) {
            // Move off empty group list
            g->isEmpty = false;

            int row = 0; // Find row where to insert
            for (int a = 0; a < m_rowTable.count(); a++) {
                if (g->orderBefore(m_rowTable[a])) {
                    row = a;
                    break;
                }
                row = a + 1;
            }

            if (notifyModel) {
                if (hasGroups()) {
                    beginInsertRows(QModelIndex(), row, row);
                } else {
                    beginInsertRows(QModelIndex(), 0, g->filtered.size());
                }
            }
            m_rowTable.insert(row, g);
            if (notifyModel) {
                endInsertRows();
            }
            m_emptyGroups.removeAll(g);
        }
    }
}

bool KateCompletionModel::indexIsItem(const QModelIndex &index) const
{
    if (!hasGroups()) {
        return true;
    }

    if (groupOfParent(index)) {
        return true;
    }

    return false;
}

void KateCompletionModel::slotModelReset()
{
    createGroups();

    // debugStats();
}

void KateCompletionModel::debugStats()
{
    if (!hasGroups()) {
        qCDebug(LOG_KTE) << "Model groupless, " << m_ungrouped->filtered.size() << " items.";
    } else {
        qCDebug(LOG_KTE) << "Model grouped (" << m_rowTable.count() << " groups):";
        for (Group *g : std::as_const(m_rowTable)) {
            qCDebug(LOG_KTE) << "Group" << g << "count" << g->filtered.size();
        }
    }
}

bool KateCompletionModel::hasCompletionModel() const
{
    return !m_completionModels.isEmpty();
}

void KateCompletionModel::setGroupingEnabled(bool enable)
{
    m_groupingEnabled = enable;
}

void KateCompletionModel::setColumnMergingEnabled(bool enable)
{
    if (m_columnMergingEnabled != enable) {
        m_columnMergingEnabled = enable;
    }
}

bool KateCompletionModel::isColumnMergingEnabled() const
{
    return m_columnMergingEnabled;
}

bool KateCompletionModel::isGroupingEnabled() const
{
    return m_groupingEnabled;
}

const QList<QList<int>> &KateCompletionModel::columnMerges() const
{
    return m_columnMerges;
}

void KateCompletionModel::setColumnMerges(const QList<QList<int>> &columnMerges)
{
    beginResetModel();
    m_columnMerges = columnMerges;
    endResetModel();
}

int KateCompletionModel::translateColumn(int sourceColumn) const
{
    if (m_columnMerges.isEmpty()) {
        return sourceColumn;
    }

    /* Debugging - dump column merge list

    QString columnMerge;
    for (const QList<int> &list : m_columnMerges) {
      columnMerge += '[';
      for (int column : list) {
        columnMerge += QString::number(column) + QLatin1Char(' ');
      }
      columnMerge += "] ";
    }

    qCDebug(LOG_KTE) << k_funcinfo << columnMerge;*/

    int c = 0;
    for (const QList<int> &list : m_columnMerges) {
        for (int column : list) {
            if (column == sourceColumn) {
                return c;
            }
        }
        c++;
    }
    return -1;
}

int KateCompletionModel::groupingAttributes(int attribute) const
{
    int ret = 0;

    if (m_groupingMethod & ScopeType) {
        if (countBits(attribute & ScopeTypeMask) > 1) {
            qCWarning(LOG_KTE) << "Invalid completion model metadata: more than one scope type modifier provided.";
        }

        if (attribute & KTextEditor::CodeCompletionModel::GlobalScope) {
            ret |= KTextEditor::CodeCompletionModel::GlobalScope;
        } else if (attribute & KTextEditor::CodeCompletionModel::NamespaceScope) {
            ret |= KTextEditor::CodeCompletionModel::NamespaceScope;
        } else if (attribute & KTextEditor::CodeCompletionModel::LocalScope) {
            ret |= KTextEditor::CodeCompletionModel::LocalScope;
        }
    }

    if (m_groupingMethod & AccessType) {
        if (countBits(attribute & AccessTypeMask) > 1) {
            qCWarning(LOG_KTE) << "Invalid completion model metadata: more than one access type modifier provided.";
        }

        if (attribute & KTextEditor::CodeCompletionModel::Public) {
            ret |= KTextEditor::CodeCompletionModel::Public;
        } else if (attribute & KTextEditor::CodeCompletionModel::Protected) {
            ret |= KTextEditor::CodeCompletionModel::Protected;
        } else if (attribute & KTextEditor::CodeCompletionModel::Private) {
            ret |= KTextEditor::CodeCompletionModel::Private;
        }

        if (accessIncludeStatic() && attribute & KTextEditor::CodeCompletionModel::Static) {
            ret |= KTextEditor::CodeCompletionModel::Static;
        }

        if (accessIncludeConst() && attribute & KTextEditor::CodeCompletionModel::Const) {
            ret |= KTextEditor::CodeCompletionModel::Const;
        }
    }

    if (m_groupingMethod & ItemType) {
        if (countBits(attribute & ItemTypeMask) > 1) {
            qCWarning(LOG_KTE) << "Invalid completion model metadata: more than one item type modifier provided.";
        }

        if (attribute & KTextEditor::CodeCompletionModel::Namespace) {
            ret |= KTextEditor::CodeCompletionModel::Namespace;
        } else if (attribute & KTextEditor::CodeCompletionModel::Class) {
            ret |= KTextEditor::CodeCompletionModel::Class;
        } else if (attribute & KTextEditor::CodeCompletionModel::Struct) {
            ret |= KTextEditor::CodeCompletionModel::Struct;
        } else if (attribute & KTextEditor::CodeCompletionModel::Union) {
            ret |= KTextEditor::CodeCompletionModel::Union;
        } else if (attribute & KTextEditor::CodeCompletionModel::Function) {
            ret |= KTextEditor::CodeCompletionModel::Function;
        } else if (attribute & KTextEditor::CodeCompletionModel::Variable) {
            ret |= KTextEditor::CodeCompletionModel::Variable;
        } else if (attribute & KTextEditor::CodeCompletionModel::Enum) {
            ret |= KTextEditor::CodeCompletionModel::Enum;
        }

        /*
        if (itemIncludeTemplate() && attribute & KTextEditor::CodeCompletionModel::Template)
          ret |= KTextEditor::CodeCompletionModel::Template;*/
    }

    return ret;
}

void KateCompletionModel::setGroupingMethod(GroupingMethods m)
{
    m_groupingMethod = m;

    createGroups();
}

bool KateCompletionModel::accessIncludeConst() const
{
    return m_accessConst;
}

void KateCompletionModel::setAccessIncludeConst(bool include)
{
    if (m_accessConst != include) {
        m_accessConst = include;

        if (groupingMethod() & AccessType) {
            createGroups();
        }
    }
}

bool KateCompletionModel::accessIncludeStatic() const
{
    return m_accessStatic;
}

void KateCompletionModel::setAccessIncludeStatic(bool include)
{
    if (m_accessStatic != include) {
        m_accessStatic = include;

        if (groupingMethod() & AccessType) {
            createGroups();
        }
    }
}

bool KateCompletionModel::accessIncludeSignalSlot() const
{
    return m_accesSignalSlot;
}

void KateCompletionModel::setAccessIncludeSignalSlot(bool include)
{
    if (m_accesSignalSlot != include) {
        m_accesSignalSlot = include;

        if (groupingMethod() & AccessType) {
            createGroups();
        }
    }
}

int KateCompletionModel::countBits(int value) const
{
    int count = 0;
    for (int i = 1; i; i <<= 1) {
        if (i & value) {
            count++;
        }
    }

    return count;
}

KateCompletionModel::GroupingMethods KateCompletionModel::groupingMethod() const
{
    return m_groupingMethod;
}

KateCompletionModel::Item::Item(bool doInitialMatch, KateCompletionModel *m, const HierarchicalModelHandler &handler, ModelRow sr)
    : model(m)
    , m_sourceRow(sr)
    , matchCompletion(StartsWithMatch)
    , m_haveExactMatch(false)
{
    inheritanceDepth = handler.getData(CodeCompletionModel::InheritanceDepth, m_sourceRow.second).toInt();
    m_unimportant = handler.getData(CodeCompletionModel::UnimportantItemRole, m_sourceRow.second).toBool();

    QModelIndex nameSibling = sr.second.sibling(sr.second.row(), CodeCompletionModel::Name);
    m_nameColumn = nameSibling.data(Qt::DisplayRole).toString();

    if (doInitialMatch) {
        match();
    }
}

bool KateCompletionModel::Item::operator<(const Item &rhs) const
{
    int ret = 0;

    // qCDebug(LOG_KTE) << c1 << " c/w " << c2 << " -> " << (model->isSortingReverse() ? ret > 0 : ret < 0) << " (" << ret << ")";

    if (m_unimportant && !rhs.m_unimportant) {
        return false;
    }

    if (!m_unimportant && rhs.m_unimportant) {
        return true;
    }

    if (matchCompletion < rhs.matchCompletion) {
        // enums are ordered in the order items should be displayed
        return true;
    }
    if (matchCompletion > rhs.matchCompletion) {
        return false;
    }

    ret = inheritanceDepth - rhs.inheritanceDepth;

    if (ret == 0) {
        auto it = rhs.model->m_currentMatch.constFind(rhs.m_sourceRow.first);
        if (it != rhs.model->m_currentMatch.cend()) {
            const QString &filter = it.value();
            bool thisStartWithFilter = m_nameColumn.startsWith(filter, Qt::CaseSensitive);
            bool rhsStartsWithFilter = rhs.m_nameColumn.startsWith(filter, Qt::CaseSensitive);

            if (thisStartWithFilter && !rhsStartsWithFilter) {
                return true;
            }
            if (rhsStartsWithFilter && !thisStartWithFilter) {
                return false;
            }
        }
    }

    if (ret == 0) {
        // Do not use localeAwareCompare, because it is simply too slow for a list of about 1000 items
        ret = QString::compare(m_nameColumn, rhs.m_nameColumn, Qt::CaseInsensitive);
    }

    if (ret == 0) {
        // FIXME need to define a better default ordering for multiple model display
        ret = m_sourceRow.second.row() - rhs.m_sourceRow.second.row();
    }

    return ret < 0;
}

void KateCompletionModel::Group::addItem(const Item &i, bool notifyModel)
{
    if (isEmpty) {
        notifyModel = false;
    }

    QModelIndex groupIndex;
    if (notifyModel) {
        groupIndex = model->indexForGroup(this);
    }

    if (notifyModel) {
        prefilter.insert(std::upper_bound(prefilter.begin(), prefilter.end(), i), i);
    } else {
        prefilter.push_back(i);
    }

    if (i.isVisible()) {
        if (notifyModel) {
            auto it = std::upper_bound(filtered.begin(), filtered.end(), i);
            const auto rowNumber = it - filtered.begin();
            model->beginInsertRows(groupIndex, rowNumber, rowNumber);
            filtered.insert(it, i);
        } else {
            // we will sort it later
            filtered.push_back(i);
        }
    }

    if (notifyModel) {
        model->endInsertRows();
    }
}

bool KateCompletionModel::Group::removeItem(const ModelRow &row)
{
    for (size_t pi = 0; pi < prefilter.size(); ++pi) {
        if (prefilter[pi].sourceRow() == row) {
            int index = rowOf(row);
            if (index != -1) {
                model->beginRemoveRows(model->indexForGroup(this), index, index);
                filtered.erase(filtered.begin() + index);
            }

            prefilter.erase(prefilter.begin() + pi);

            if (index != -1) {
                model->endRemoveRows();
            }

            return index != -1;
        }
    }

    Q_ASSERT(false);
    return false;
}

KateCompletionModel::Group::Group(const QString &title, int attribute, KateCompletionModel *m)
    : model(m)
    , attribute(attribute)
    // ugly hack to add some left margin
    , title(QLatin1Char(' ') + title)
    , isEmpty(true)
    , customSortingKey(-1)
{
    Q_ASSERT(model);
}

void KateCompletionModel::Group::resort()
{
    std::stable_sort(filtered.begin(), filtered.end());
    model->hideOrShowGroup(this);
}

void KateCompletionModel::resort()
{
    for (Group *g : std::as_const(m_rowTable)) {
        g->resort();
    }

    for (Group *g : std::as_const(m_emptyGroups)) {
        g->resort();
    }

    // call updateBestMatches here, so they are moved to the top again.
    updateBestMatches();
}

bool KateCompletionModel::Item::isValid() const
{
    return model && m_sourceRow.first && m_sourceRow.second.row() >= 0;
}

void KateCompletionModel::Group::clear()
{
    prefilter.clear();
    filtered.clear();
    isEmpty = true;
}

uint KateCompletionModel::filteredItemCount() const
{
    uint ret = 0;
    for (Group *group : m_rowTable) {
        ret += group->filtered.size();
    }

    return ret;
}

bool KateCompletionModel::shouldMatchHideCompletionList() const
{
    // @todo Make this faster

    bool doHide = false;
    CodeCompletionModel *hideModel = nullptr;

    for (Group *group : std::as_const(m_rowTable)) {
        for (const Item &item : std::as_const(group->filtered)) {
            if (item.haveExactMatch()) {
                KTextEditor::CodeCompletionModelControllerInterface *iface3 =
                    dynamic_cast<KTextEditor::CodeCompletionModelControllerInterface *>(item.sourceRow().first);
                bool hide = false;
                if (!iface3) {
                    hide = true;
                }
                if (iface3
                    && iface3->matchingItem(item.sourceRow().second) == KTextEditor::CodeCompletionModelControllerInterface::HideListIfAutomaticInvocation) {
                    hide = true;
                }
                if (hide) {
                    doHide = true;
                    hideModel = item.sourceRow().first;
                }
            }
        }
    }

    if (doHide) {
        // Check if all other visible items are from the same model
        for (Group *group : std::as_const(m_rowTable)) {
            for (const Item &item : std::as_const(group->filtered)) {
                if (item.sourceRow().first != hideModel) {
                    return false;
                }
            }
        }
    }

    return doHide;
}

static inline QChar toLowerIfInsensitive(QChar c, Qt::CaseSensitivity caseSensitive)
{
    if (caseSensitive != Qt::CaseInsensitive) {
        return c;
    }
    return c.isLower() ? c : c.toLower();
}

static inline bool matchesAbbreviationHelper(const QString &word,
                                             const QString &typed,
                                             const QVarLengthArray<int, 32> &offsets,
                                             Qt::CaseSensitivity caseSensitive,
                                             int &depth,
                                             int atWord = -1,
                                             int i = 0)
{
    int atLetter = 1;
    for (; i < typed.size(); i++) {
        const QChar c = toLowerIfInsensitive(typed.at(i), caseSensitive);
        bool haveNextWord = offsets.size() > atWord + 1;
        bool canCompare = atWord != -1 && word.size() > offsets.at(atWord) + atLetter;
        if (canCompare && c == toLowerIfInsensitive(word.at(offsets.at(atWord) + atLetter), caseSensitive)) {
            // the typed letter matches a letter after the current word beginning
            if (!haveNextWord || c != toLowerIfInsensitive(word.at(offsets.at(atWord + 1)), caseSensitive)) {
                // good, simple case, no conflict
                atLetter += 1;
                continue;
            }
            // For maliciously crafted data, the code used here theoretically can have very high
            // complexity. Thus ensure we don't run into this case, by limiting the amount of branches
            // we walk through to 128.
            depth++;
            if (depth > 128) {
                return false;
            }
            // the letter matches both the next word beginning and the next character in the word
            if (haveNextWord && matchesAbbreviationHelper(word, typed, offsets, caseSensitive, depth, atWord + 1, i + 1)) {
                // resolving the conflict by taking the next word's first character worked, fine
                return true;
            }
            // otherwise, continue by taking the next letter in the current word.
            atLetter += 1;
            continue;
        } else if (haveNextWord && c == toLowerIfInsensitive(word.at(offsets.at(atWord + 1)), caseSensitive)) {
            // the typed letter matches the next word beginning
            atWord++;
            atLetter = 1;
            continue;
        }
        // no match
        return false;
    }
    // all characters of the typed word were matched
    return true;
}

bool KateCompletionModel::matchesAbbreviation(const QString &word, const QString &typed, Qt::CaseSensitivity caseSensitive)
{
    // A mismatch is very likely for random even for the first letter,
    // thus this optimization makes sense.
    if (toLowerIfInsensitive(word.at(0), caseSensitive) != toLowerIfInsensitive(typed.at(0), caseSensitive)) {
        return false;
    }

    // First, check if all letters are contained in the word in the right order.
    int atLetter = 0;
    for (const QChar c : typed) {
        while (toLowerIfInsensitive(c, caseSensitive) != toLowerIfInsensitive(word.at(atLetter), caseSensitive)) {
            atLetter += 1;
            if (atLetter >= word.size()) {
                return false;
            }
        }
    }

    bool haveUnderscore = true;
    QVarLengthArray<int, 32> offsets;
    // We want to make "KComplM" match "KateCompletionModel"; this means we need
    // to allow parts of the typed text to be not part of the actual abbreviation,
    // which consists only of the uppercased / underscored letters (so "KCM" in this case).
    // However it might be ambiguous whether a letter is part of such a word or part of
    // the following abbreviation, so we need to find all possible word offsets first,
    // then compare.
    for (int i = 0; i < word.size(); i++) {
        const QChar c = word.at(i);
        if (c == QLatin1Char('_')) {
            haveUnderscore = true;
        } else if (haveUnderscore || c.isUpper()) {
            offsets.append(i);
            haveUnderscore = false;
        }
    }
    int depth = 0;
    return matchesAbbreviationHelper(word, typed, offsets, caseSensitive, depth);
}

static inline bool containsAtWordBeginning(const QString &word, const QString &typed, Qt::CaseSensitivity caseSensitive)
{
    if (typed.size() > word.size()) {
        return false;
    }

    for (int i = 1; i < word.size(); i++) {
        // The current position is a word beginning if the previous character was an underscore
        // or if the current character is uppercase. Subsequent uppercase characters do not count,
        // to handle the special case of UPPER_CASE_VARS properly.
        const QChar c = word.at(i);
        const QChar prev = word.at(i - 1);
        if (!(prev == QLatin1Char('_') || (c.isUpper() && !prev.isUpper()))) {
            continue;
        }
        if (QStringView(word).mid(i).startsWith(typed, caseSensitive)) {
            return true;
        }

        // If we do not have enough string left, return early
        if (word.size() - i < typed.size()) {
            return false;
        }
    }
    return false;
}

KateCompletionModel::Item::MatchType KateCompletionModel::Item::match()
{
    QString match = model->currentCompletion(m_sourceRow.first);

    m_haveExactMatch = false;

    // Hehe, everything matches nothing! (ie. everything matches a blank string)
    if (match.isEmpty()) {
        return PerfectMatch;
    }
    if (m_nameColumn.isEmpty()) {
        return NoMatch;
    }

    matchCompletion = (m_nameColumn.startsWith(match, model->matchCaseSensitivity()) ? StartsWithMatch : NoMatch);
    if (matchCompletion == NoMatch) {
        // if no match, try for "contains"
        // Only match when the occurrence is at a "word" beginning, marked by
        // an underscore or a capital. So Foo matches BarFoo and Bar_Foo, but not barfoo.
        // Starting at 1 saves looking at the beginning of the word, that was already checked above.
        if (containsAtWordBeginning(m_nameColumn, match, model->matchCaseSensitivity())) {
            matchCompletion = ContainsMatch;
        }
    }

    if (matchCompletion == NoMatch && !m_nameColumn.isEmpty() && !match.isEmpty()) {
        // if still no match, try abbreviation matching
        if (matchesAbbreviation(m_nameColumn, match, model->matchCaseSensitivity())) {
            matchCompletion = AbbreviationMatch;
        }
    }

    if (matchCompletion && match.length() == m_nameColumn.length()) {
        if (model->matchCaseSensitivity() == Qt::CaseInsensitive && model->exactMatchCaseSensitivity() == Qt::CaseSensitive
            && !m_nameColumn.startsWith(match, Qt::CaseSensitive)) {
            return matchCompletion;
        }
        matchCompletion = PerfectMatch;
        m_haveExactMatch = true;
    }

    return matchCompletion;
}

bool KateCompletionModel::Item::isVisible() const
{
    return matchCompletion;
}

const KateCompletionModel::ModelRow &KateCompletionModel::Item::sourceRow() const
{
    return m_sourceRow;
}

QString KateCompletionModel::currentCompletion(KTextEditor::CodeCompletionModel *model) const
{
    return m_currentMatch.value(model);
}

Qt::CaseSensitivity KateCompletionModel::matchCaseSensitivity() const
{
    return m_matchCaseSensitivity;
}

Qt::CaseSensitivity KateCompletionModel::exactMatchCaseSensitivity() const
{
    return m_exactMatchCaseSensitivity;
}

void KateCompletionModel::addCompletionModel(KTextEditor::CodeCompletionModel *model)
{
    if (m_completionModels.contains(model)) {
        return;
    }

    m_completionModels.append(model);

    connect(model, &KTextEditor::CodeCompletionModel::rowsInserted, this, &KateCompletionModel::slotRowsInserted);
    connect(model, &KTextEditor::CodeCompletionModel::rowsRemoved, this, &KateCompletionModel::slotRowsRemoved);
    connect(model, &KTextEditor::CodeCompletionModel::modelReset, this, &KateCompletionModel::slotModelReset);

    // This performs the reset
    createGroups();
}

void KateCompletionModel::setCompletionModel(KTextEditor::CodeCompletionModel *model)
{
    clearCompletionModels();
    addCompletionModel(model);
}

void KateCompletionModel::setCompletionModels(const QList<KTextEditor::CodeCompletionModel *> &models)
{
    // if (m_completionModels == models)
    // return;

    clearCompletionModels();

    m_completionModels = models;

    for (KTextEditor::CodeCompletionModel *model : models) {
        connect(model, &KTextEditor::CodeCompletionModel::rowsInserted, this, &KateCompletionModel::slotRowsInserted);
        connect(model, &KTextEditor::CodeCompletionModel::rowsRemoved, this, &KateCompletionModel::slotRowsRemoved);
        connect(model, &KTextEditor::CodeCompletionModel::modelReset, this, &KateCompletionModel::slotModelReset);
    }

    // This performs the reset
    createGroups();
}

QList<KTextEditor::CodeCompletionModel *> KateCompletionModel::completionModels() const
{
    return m_completionModels;
}

void KateCompletionModel::removeCompletionModel(CodeCompletionModel *model)
{
    if (!model || !m_completionModels.contains(model)) {
        return;
    }

    beginResetModel();
    m_currentMatch.remove(model);

    clearGroups();

    model->disconnect(this);

    m_completionModels.removeAll(model);
    endResetModel();

    if (!m_completionModels.isEmpty()) {
        // This performs the reset
        createGroups();
    }
}

void KateCompletionModel::makeGroupItemsUnique(bool onlyFiltered)
{
    struct FilterItems {
        FilterItems(KateCompletionModel &model, const QVector<KTextEditor::CodeCompletionModel *> &needShadowing)
            : m_model(model)
            , m_needShadowing(needShadowing)
        {
        }

        QHash<QString, CodeCompletionModel *> had;
        KateCompletionModel &m_model;
        const QVector<KTextEditor::CodeCompletionModel *> &m_needShadowing;

        void filter(std::vector<Item> &items)
        {
            std::vector<Item> temp;
            temp.reserve(items.size());
            had.reserve(items.size());
            for (const Item &item : items) {
                auto it = had.constFind(item.name());
                if (it != had.constEnd() && *it != item.sourceRow().first && m_needShadowing.contains(item.sourceRow().first)) {
                    continue;
                }
                had.insert(item.name(), item.sourceRow().first);
                temp.push_back(item);
            }
            items.swap(temp);
        }

        void filter(Group *group, bool onlyFiltered)
        {
            if (group->prefilter.size() == group->filtered.size()) {
                // Filter only once
                filter(group->filtered);
                if (!onlyFiltered) {
                    group->prefilter = group->filtered;
                }
            } else {
                // Must filter twice
                filter(group->filtered);
                if (!onlyFiltered) {
                    filter(group->prefilter);
                }
            }

            if (group->filtered.empty()) {
                m_model.hideOrShowGroup(group);
            }
        }
    };

    QVector<KTextEditor::CodeCompletionModel *> needShadowing;
    for (KTextEditor::CodeCompletionModel *model : std::as_const(m_completionModels)) {
        KTextEditor::CodeCompletionModelControllerInterface *v4 = dynamic_cast<KTextEditor::CodeCompletionModelControllerInterface *>(model);
        if (v4 && v4->shouldHideItemsWithEqualNames()) {
            needShadowing.push_back(model);
        }
    }

    if (needShadowing.isEmpty()) {
        return;
    }

    FilterItems filter(*this, needShadowing);

    filter.filter(m_ungrouped, onlyFiltered);

    for (Group *group : std::as_const(m_rowTable)) {
        filter.filter(group, onlyFiltered);
    }
}

// Updates the best-matches group
void KateCompletionModel::updateBestMatches()
{
    // We cannot do too many operations here, because they are all executed
    // whenever a character is added. Would be nice if we could split the
    // operations up somewhat using a timer.
    int maxMatches = 300;

    m_updateBestMatchesTimer->stop();
    // Maps match-qualities to ModelRows paired together with the BestMatchesCount returned by the items.
    typedef QMultiMap<int, QPair<int, ModelRow>> BestMatchMap;
    BestMatchMap matches;

    if (!hasGroups()) {
        // If there is no grouping, just change the order of the items, moving the best matching ones to the front
        QMultiMap<int, int> rowsForQuality;

        int row = 0;
        for (const Item &item : m_ungrouped->filtered) {
            ModelRow source = item.sourceRow();

            QVariant v = source.second.data(CodeCompletionModel::BestMatchesCount);

            if (v.type() == QVariant::Int && v.toInt() > 0) {
                int quality = contextMatchQuality(source);
                if (quality > 0) {
                    rowsForQuality.insert(quality, row);
                }
            }

            ++row;
            --maxMatches;
            if (maxMatches < 0) {
                break;
            }
        }

        if (!rowsForQuality.isEmpty()) {
            // Rewrite m_ungrouped->filtered in a new order
            QSet<int> movedToFront;
            std::vector<Item> newFiltered;
            newFiltered.reserve(rowsForQuality.size());
            movedToFront.reserve(rowsForQuality.size());
            for (auto it = rowsForQuality.constBegin(); it != rowsForQuality.constEnd(); ++it) {
                newFiltered.push_back(m_ungrouped->filtered[it.value()]);
                movedToFront.insert(it.value());
            }
            std::reverse(newFiltered.begin(), newFiltered.end());

            int size = m_ungrouped->filtered.size();
            for (int a = 0; a < size; ++a) {
                if (!movedToFront.contains(a)) {
                    newFiltered.push_back(m_ungrouped->filtered[a]);
                }
            }
            m_ungrouped->filtered.swap(newFiltered);
        }
        return;
    }

    ///@todo Cache the CodeCompletionModel::BestMatchesCount
    for (Group *g : std::as_const(m_rowTable)) {
        if (g == m_bestMatches) {
            continue;
        }
        for (int a = 0; a < (int)g->filtered.size(); a++) {
            ModelRow source = g->filtered[a].sourceRow();

            QVariant v = source.second.data(CodeCompletionModel::BestMatchesCount);

            if (v.type() == QVariant::Int && v.toInt() > 0) {
                // Return the best match with any of the argument-hints

                int quality = contextMatchQuality(source);
                if (quality > 0) {
                    matches.insert(quality, qMakePair(v.toInt(), g->filtered[a].sourceRow()));
                }
                --maxMatches;
            }

            if (maxMatches < 0) {
                break;
            }
        }
        if (maxMatches < 0) {
            break;
        }
    }

    // Now choose how many of the matches will be taken. This is done with the rule:
    // The count of shown best-matches should equal the average count of their BestMatchesCounts
    int cnt = 0;
    int matchesSum = 0;
    BestMatchMap::const_iterator it = matches.constEnd();
    while (it != matches.constBegin()) {
        --it;
        ++cnt;
        matchesSum += (*it).first;
        if (cnt > matchesSum / cnt) {
            break;
        }
    }

    m_bestMatches->filtered.clear();

    it = matches.constEnd();

    while (it != matches.constBegin() && cnt > 0) {
        --it;
        --cnt;

        m_bestMatches->filtered.push_back(Item(true, this, HierarchicalModelHandler((*it).second.first), (*it).second));
    }

    hideOrShowGroup(m_bestMatches);
}

void KateCompletionModel::rowSelected(const QModelIndex &row)
{
    ExpandingWidgetModel::rowSelected(row);
    ///@todo delay this
    int rc = widget()->argumentHintModel()->rowCount(QModelIndex());
    if (rc == 0) {
        return;
    }

    // For now, simply update the whole column 0
    QModelIndex start = widget()->argumentHintModel()->index(0, 0);
    QModelIndex end = widget()->argumentHintModel()->index(rc - 1, 0);

    widget()->argumentHintModel()->emitDataChanged(start, end);
}

void KateCompletionModel::clearCompletionModels()
{
    if (m_completionModels.isEmpty()) {
        return;
    }

    beginResetModel();
    for (CodeCompletionModel *model : std::as_const(m_completionModels)) {
        model->disconnect(this);
    }

    m_completionModels.clear();

    m_currentMatch.clear();

    clearGroups();
    endResetModel();
}
