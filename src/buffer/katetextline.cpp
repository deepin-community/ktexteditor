/*
    SPDX-FileCopyrightText: 2010 Christoph Cullmann <cullmann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katetextline.h"

namespace Kate
{

int TextLineData::firstChar() const
{
    return nextNonSpaceChar(0);
}

int TextLineData::lastChar() const
{
    return previousNonSpaceChar(m_text.length() - 1);
}

int TextLineData::nextNonSpaceChar(int pos) const
{
    Q_ASSERT(pos >= 0);

    for (int i = pos; i < m_text.length(); i++) {
        if (!m_text[i].isSpace()) {
            return i;
        }
    }

    return -1;
}

int TextLineData::previousNonSpaceChar(int pos) const
{
    if (pos >= m_text.length()) {
        pos = m_text.length() - 1;
    }

    for (int i = pos; i >= 0; i--) {
        if (!m_text[i].isSpace()) {
            return i;
        }
    }

    return -1;
}

QString TextLineData::leadingWhitespace() const
{
    if (firstChar() < 0) {
        return string(0, length());
    }

    return string(0, firstChar());
}

int TextLineData::indentDepth(int tabWidth) const
{
    int d = 0;
    const int len = m_text.length();
    const QChar *unicode = m_text.unicode();

    for (int i = 0; i < len; ++i) {
        if (unicode[i].isSpace()) {
            if (unicode[i] == QLatin1Char('\t')) {
                d += tabWidth - (d % tabWidth);
            } else {
                d++;
            }
        } else {
            return d;
        }
    }

    return d;
}

bool TextLineData::matchesAt(int column, const QString &match) const
{
    if (column < 0) {
        return false;
    }

    const int len = m_text.length();
    const int matchlen = match.length();

    if ((column + matchlen) > len) {
        return false;
    }

    const QChar *unicode = m_text.unicode();
    const QChar *matchUnicode = match.unicode();

    for (int i = 0; i < matchlen; ++i) {
        if (unicode[i + column] != matchUnicode[i]) {
            return false;
        }
    }

    return true;
}

int TextLineData::toVirtualColumn(int column, int tabWidth) const
{
    if (column < 0) {
        return 0;
    }

    int x = 0;
    const int zmax = qMin(column, m_text.length());
    const QChar *unicode = m_text.unicode();

    for (int z = 0; z < zmax; ++z) {
        if (unicode[z] == QLatin1Char('\t')) {
            x += tabWidth - (x % tabWidth);
        } else {
            x++;
        }
    }

    return x + column - zmax;
}

int TextLineData::fromVirtualColumn(int column, int tabWidth) const
{
    if (column < 0) {
        return 0;
    }

    const int zmax = qMin(m_text.length(), column);
    const QChar *unicode = m_text.unicode();

    int x = 0;
    int z = 0;
    for (; z < zmax; ++z) {
        int diff = 1;
        if (unicode[z] == QLatin1Char('\t')) {
            diff = tabWidth - (x % tabWidth);
        }

        if (x + diff > column) {
            break;
        }
        x += diff;
    }

    return z + qMax(column - x, 0);
}

int TextLineData::virtualLength(int tabWidth) const
{
    int x = 0;
    const int len = m_text.length();
    const QChar *unicode = m_text.unicode();

    for (int z = 0; z < len; ++z) {
        if (unicode[z] == QLatin1Char('\t')) {
            x += tabWidth - (x % tabWidth);
        } else {
            x++;
        }
    }

    return x;
}

void TextLineData::addAttribute(const Attribute &attribute)
{
    // try to append to previous range, if same attribute value
    if (!m_attributesList.isEmpty() && (m_attributesList.back().attributeValue == attribute.attributeValue)
        && ((m_attributesList.back().offset + m_attributesList.back().length) == attribute.offset)) {
        m_attributesList.back().length += attribute.length;
        return;
    }

    m_attributesList.append(attribute);
}

short TextLineData::attribute(int pos) const
{
    auto found = std::upper_bound(m_attributesList.cbegin(), m_attributesList.cend(), pos, [](const int &p, const Attribute &x) {
        return p < x.offset + x.length;
    });
    if (found != m_attributesList.cend() && found->offset <= pos && pos < (found->offset + found->length)) {
        return found->attributeValue;
    }
    return 0;
}

}
