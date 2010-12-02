/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "InspectorStyleSheet.h"

#if ENABLE(INSPECTOR)

#include "CSSParser.h"
#include "CSSPropertySourceData.h"
#include "CSSRule.h"
#include "CSSRuleList.h"
#include "CSSStyleRule.h"
#include "CSSStyleSelector.h"
#include "CSSStyleSheet.h"
#include "Document.h"
#include "Element.h"
#include "HTMLHeadElement.h"
#include "InspectorCSSAgent.h"
#include "InspectorResourceAgent.h"
#include "InspectorValues.h"
#include "Node.h"
#include "StyleSheetList.h"

#include <wtf/OwnPtr.h>
#include <wtf/PassOwnPtr.h>
#include <wtf/Vector.h>

class ParsedStyleSheet {
public:
    typedef Vector<RefPtr<WebCore::CSSRuleSourceData> > SourceData;
    ParsedStyleSheet();

    WebCore::CSSStyleSheet* cssStyleSheet() const { return m_parserOutput; }
    const String& text() const { return m_text; }
    void setText(const String& text);
    bool hasText() const { return m_hasText; }
    SourceData* sourceData() const { return m_sourceData.get(); }
    void setSourceData(PassOwnPtr<SourceData> sourceData);
    bool hasSourceData() const { return m_sourceData; }
    RefPtr<WebCore::CSSRuleSourceData> ruleSourceDataAt(unsigned index) const;

private:

    // StyleSheet constructed while parsing m_text.
    WebCore::CSSStyleSheet* m_parserOutput;
    String m_text;
    bool m_hasText;
    OwnPtr<SourceData> m_sourceData;
};

ParsedStyleSheet::ParsedStyleSheet()
    : m_parserOutput(0)
    , m_hasText(false)
{
}

void ParsedStyleSheet::setText(const String& text)
{
    m_hasText = true;
    m_text = text;
    setSourceData(0);
}

void ParsedStyleSheet::setSourceData(PassOwnPtr<SourceData> sourceData)
{
    m_sourceData = sourceData;
}

RefPtr<WebCore::CSSRuleSourceData> ParsedStyleSheet::ruleSourceDataAt(unsigned index) const
{
    if (!hasSourceData() || index >= m_sourceData->size())
        return 0;

    return m_sourceData->at(index);
}

namespace WebCore {

PassRefPtr<InspectorObject> InspectorStyle::buildObjectForStyle() const
{
    RefPtr<InspectorObject> result = InspectorObject::create();
    if (!m_styleId.isEmpty())
        result->setString("styleId", m_styleId.asString());

    RefPtr<InspectorObject> propertiesObject = InspectorObject::create();
    propertiesObject->setString("width", m_style->getPropertyValue("width"));
    propertiesObject->setString("height", m_style->getPropertyValue("height"));

    RefPtr<CSSRuleSourceData> sourceData = m_parentStyleSheet ? m_parentStyleSheet->ruleSourceDataFor(m_style) : 0;
    if (sourceData) {
        propertiesObject->setNumber("startOffset", sourceData->styleSourceData->styleBodyRange.start);
        propertiesObject->setNumber("endOffset", sourceData->styleSourceData->styleBodyRange.end);
    }
    result->setObject("properties", propertiesObject);

    populateObjectWithStyleProperties(result.get());

    return result.release();
}

bool InspectorStyle::setPropertyText(unsigned index, const String& propertyText, bool overwrite)
{
    ASSERT(m_parentStyleSheet);
    if (!m_parentStyleSheet->ensureParsedDataReady())
        return false;

    Vector<InspectorStyleProperty> allProperties;
    populateAllProperties(&allProperties);

    unsigned propertyStart = 0; // Need to initialize to make the compiler happy.
    long propertyLengthDelta;

    if (overwrite) {
        ASSERT(index < allProperties.size());
        InspectorStyleProperty& property = allProperties.at(index);
        propertyStart = property.sourceData.range.start;
        unsigned propertyEnd = property.sourceData.range.end;
        unsigned oldLength = propertyEnd - propertyStart;
        unsigned newLength = propertyText.length();
        propertyLengthDelta = newLength - oldLength;

        if (!property.disabled) {
            bool success = replacePropertyInStyleText(property, propertyText);
            if (!success)
                return false;
        } else {
            property.rawText = propertyText;
            if (!propertyText.length()) {
                bool success = enableProperty(index, allProperties);
                return success;
            }
        }
    } else {
        // Insert at index.
        RefPtr<CSSRuleSourceData> sourceData = m_parentStyleSheet->ruleSourceDataFor(m_style);
        if (!sourceData)
            return false;
        String text;
        bool success = styleText(&text);
        if (!success)
            return false;
        propertyLengthDelta = propertyText.length();

        bool insertLast = true;
        if (index < allProperties.size()) {
            InspectorStyleProperty& property = allProperties.at(index);
            if (property.hasSource) {
                propertyStart = property.sourceData.range.start;
                // If inserting before a disabled property, it should be shifted, too.
                insertLast = false;
            }
        }
        if (insertLast)
            propertyStart = sourceData->styleSourceData->styleBodyRange.end;

        text.insert(propertyText, propertyStart);
        m_parentStyleSheet->setStyleText(m_style, text);
    }

    // Recompute subsequent disabled property ranges.
    shiftDisabledProperties(disabledIndexByOrdinal(index, true, allProperties), propertyLengthDelta);

    return true;
}

bool InspectorStyle::toggleProperty(unsigned index, bool disable)
{
    ASSERT(m_parentStyleSheet);
    if (!m_parentStyleSheet->ensureParsedDataReady())
        return false; // Can toggle only source-based properties.
    RefPtr<CSSRuleSourceData> sourceData = m_parentStyleSheet->ruleSourceDataFor(m_style);
    if (!sourceData)
        return false; // No source data for the style.

    Vector<InspectorStyleProperty> allProperties;
    populateAllProperties(&allProperties);
    if (index >= allProperties.size())
        return false; // Outside of property range.

    InspectorStyleProperty& property = allProperties.at(index);
    if (property.disabled == disable)
        return true; // Idempotent operation.

    bool success;
    if (!disable)
        success = enableProperty(index, allProperties);
    else
        success = disableProperty(index, allProperties);

    return success;
}

// static
unsigned InspectorStyle::disabledIndexByOrdinal(unsigned ordinal, bool canUseSubsequent, Vector<InspectorStyleProperty>& allProperties)
{
    unsigned disabledIndex = 0;
    for (unsigned i = 0, size = allProperties.size(); i < size; ++i) {
        InspectorStyleProperty& property = allProperties.at(i);
        if (property.disabled) {
            if (i == ordinal || (canUseSubsequent && i > ordinal))
                return disabledIndex;
            ++disabledIndex;
        }
    }

    return UINT_MAX;
}

bool InspectorStyle::styleText(String* result)
{
    // Precondition: m_parentStyleSheet->ensureParsedDataReady() has been called successfully.
    RefPtr<CSSRuleSourceData> sourceData = m_parentStyleSheet->ruleSourceDataFor(m_style);
    if (!sourceData)
        return false;

    String styleSheetText;
    bool success = m_parentStyleSheet->text(&styleSheetText);
    if (!success)
        return false;

    SourceRange& bodyRange = sourceData->styleSourceData->styleBodyRange;
    *result = styleSheetText.substring(bodyRange.start, bodyRange.end - bodyRange.start);
    return true;
}

bool InspectorStyle::disableProperty(unsigned indexToDisable, Vector<InspectorStyleProperty>& allProperties)
{
    // Precondition: |indexToEnable| points to an enabled property.
    const InspectorStyleProperty& property = allProperties.at(indexToDisable);
    InspectorStyleProperty disabledProperty(property);
    disabledProperty.disabled = true;
    unsigned propertyStart = property.sourceData.range.start;
    // This may have to be negated below.
    long propertyLength = property.sourceData.range.end - propertyStart;
    disabledProperty.sourceData.range.end = propertyStart;
    String oldStyleText;
    bool success = styleText(&oldStyleText);
    if (!success)
        return false;
    disabledProperty.rawText = oldStyleText.substring(propertyStart, propertyLength);
    success = replacePropertyInStyleText(property, "");
    if (!success)
        return false;

    // Add disabled property at correct position.
    unsigned insertionIndex = disabledIndexByOrdinal(indexToDisable, true, allProperties);
    if (insertionIndex == UINT_MAX)
        m_disabledProperties.append(disabledProperty);
    else {
        m_disabledProperties.insert(insertionIndex, disabledProperty);
        shiftDisabledProperties(insertionIndex + 1, -propertyLength); // Property removed from text - shift these back.
    }
    return true;
}

bool InspectorStyle::enableProperty(unsigned indexToEnable, Vector<InspectorStyleProperty>& allProperties)
{
    // Precondition: |indexToEnable| points to a disabled property.
    unsigned disabledIndex = disabledIndexByOrdinal(indexToEnable, false, allProperties);
    if (disabledIndex == UINT_MAX)
        return false;

    InspectorStyleProperty disabledProperty = m_disabledProperties.at(disabledIndex);
    m_disabledProperties.remove(disabledIndex);
    bool success = replacePropertyInStyleText(disabledProperty, disabledProperty.rawText);
    if (success)
        shiftDisabledProperties(disabledIndex, disabledProperty.rawText.length());
    return success;
}

bool InspectorStyle::populateAllProperties(Vector<InspectorStyleProperty>* result) const
{
    HashSet<String> foundShorthands;
    HashSet<String> sourcePropertyNames;
    unsigned disabledIndex = 0;
    unsigned disabledLength = m_disabledProperties.size();
    InspectorStyleProperty disabledProperty;
    if (disabledIndex < disabledLength)
        disabledProperty = m_disabledProperties.at(disabledIndex);

    RefPtr<CSSRuleSourceData> sourceData = (m_parentStyleSheet && m_parentStyleSheet->ensureParsedDataReady()) ? m_parentStyleSheet->ruleSourceDataFor(m_style) : 0;
    Vector<CSSPropertySourceData>* sourcePropertyData = sourceData ? &(sourceData->styleSourceData->propertyData) : 0;
    if (sourcePropertyData) {
        for (Vector<CSSPropertySourceData>::const_iterator it = sourcePropertyData->begin(); it != sourcePropertyData->end(); ++it) {
            while (disabledIndex < disabledLength && disabledProperty.sourceData.range.start <= it->range.start) {
                result->append(disabledProperty);
                if (++disabledIndex < disabledLength)
                    disabledProperty = m_disabledProperties.at(disabledIndex);
            }
            result->append(InspectorStyleProperty(*it, true, false));
            sourcePropertyNames.add(it->name);
        }
    }

    while (disabledIndex < disabledLength) {
        disabledProperty = m_disabledProperties.at(disabledIndex++);
        result->append(disabledProperty);
    }

    for (int i = 0, size = m_style->length(); i < size; ++i) {
        String name = m_style->item(i);
        if (sourcePropertyNames.contains(name))
            continue;

        sourcePropertyNames.add(name);
        result->append(InspectorStyleProperty(CSSPropertySourceData(name, m_style->getPropertyValue(name), !m_style->getPropertyPriority(name).isEmpty(), true, SourceRange()), false, false));
    }

    return true;
}

void InspectorStyle::populateObjectWithStyleProperties(InspectorObject* result) const
{
    Vector<InspectorStyleProperty> properties;
    populateAllProperties(&properties);

    RefPtr<InspectorArray> propertiesObject = InspectorArray::create();
    RefPtr<InspectorObject> shorthandValues = InspectorObject::create();
    HashMap<String, RefPtr<InspectorObject> > propertyNameToPreviousActiveProperty;
    HashSet<String> foundShorthands;

    for (Vector<InspectorStyleProperty>::iterator it = properties.begin(), itEnd = properties.end(); it != itEnd; ++it) {
        const CSSPropertySourceData& propertyEntry = it->sourceData;
        const String& name = propertyEntry.name;

        RefPtr<InspectorObject> property = InspectorObject::create();
        propertiesObject->pushObject(property);
        property->setString("status", it->disabled ? "disabled" : "active");
        property->setBoolean("parsedOk", propertyEntry.parsedOk);
        if (!it->disabled) {
            property->setString("name", name);
            property->setString("value", propertyEntry.value);
            property->setString("priority", propertyEntry.important ? "important" : "");
            if (it->hasSource) {
                property->setBoolean("implicit", false);
                property->setNumber("startOffset", propertyEntry.range.start);
                property->setNumber("endOffset", propertyEntry.range.end);

                HashMap<String, RefPtr<InspectorObject> >::iterator activeIt = propertyNameToPreviousActiveProperty.find(name);
                if (activeIt != propertyNameToPreviousActiveProperty.end()) {
                    activeIt->second->setString("status", "inactive");
                    activeIt->second->setString("shorthandName", "");
                }
                propertyNameToPreviousActiveProperty.set(name, property);
            } else {
                property->setBoolean("implicit", m_style->isPropertyImplicit(name));
                property->setString("status", "style");
            }
        } else
            property->setString("text", it->rawText);

        if (propertyEntry.parsedOk) {
            // Both for style-originated and parsed source properties.
            String shorthand = m_style->getPropertyShorthand(name);
            property->setString("shorthandName", shorthand);
            if (!shorthand.isEmpty() && !foundShorthands.contains(shorthand)) {
                foundShorthands.add(shorthand);
                shorthandValues->setString(shorthand, shorthandValue(shorthand));
            }
        } else
            property->setString("shorthandName", "");
    }

    result->setArray("cssProperties", propertiesObject);
    result->setObject("shorthandValues", shorthandValues);
}


void InspectorStyle::shiftDisabledProperties(unsigned fromIndex, long delta)
{
    for (unsigned i = fromIndex, size = m_disabledProperties.size(); i < size; ++i) {
        SourceRange& range = m_disabledProperties.at(i).sourceData.range;
        range.start += delta;
        range.end += delta;
    }
}

bool InspectorStyle::replacePropertyInStyleText(const InspectorStyleProperty& property, const String& newText)
{
    // Precondition: m_parentStyleSheet->ensureParsedDataReady() has been called successfully.
    String text;
    bool success = styleText(&text);
    if (!success)
        return false;
    const SourceRange& range = property.sourceData.range;
    text.replace(range.start, range.end - range.start, newText);
    success = m_parentStyleSheet->setStyleText(m_style, text);
    return success;
}

String InspectorStyle::shorthandValue(const String& shorthandProperty) const
{
    String value = m_style->getPropertyValue(shorthandProperty);
    if (value.isEmpty()) {
        for (unsigned i = 0; i < m_style->length(); ++i) {
            String individualProperty = m_style->item(i);
            if (m_style->getPropertyShorthand(individualProperty) != shorthandProperty)
                continue;
            if (m_style->isPropertyImplicit(individualProperty))
                continue;
            String individualValue = m_style->getPropertyValue(individualProperty);
            if (individualValue == "initial")
                continue;
            if (value.length())
                value.append(" ");
            value.append(individualValue);
        }
    }
    return value;
}

String InspectorStyle::shorthandPriority(const String& shorthandProperty) const
{
    String priority = m_style->getPropertyPriority(shorthandProperty);
    if (priority.isEmpty()) {
        for (unsigned i = 0; i < m_style->length(); ++i) {
            String individualProperty = m_style->item(i);
            if (m_style->getPropertyShorthand(individualProperty) != shorthandProperty)
                continue;
            priority = m_style->getPropertyPriority(individualProperty);
            break;
        }
    }
    return priority;
}

Vector<String> InspectorStyle::longhandProperties(const String& shorthandProperty) const
{
    Vector<String> properties;
    HashSet<String> foundProperties;
    for (unsigned i = 0; i < m_style->length(); ++i) {
        String individualProperty = m_style->item(i);
        if (foundProperties.contains(individualProperty) || m_style->getPropertyShorthand(individualProperty) != shorthandProperty)
            continue;

        foundProperties.add(individualProperty);
        properties.append(individualProperty);
    }
    return properties;
}

InspectorStyleSheet::InspectorStyleSheet(const String& id, CSSStyleSheet* pageStyleSheet, const String& origin, const String& documentURL)
    : m_id(id)
    , m_pageStyleSheet(pageStyleSheet)
    , m_origin(origin)
    , m_documentURL(documentURL)
    , m_isRevalidating(false)
{
    m_parsedStyleSheet = new ParsedStyleSheet();
}

InspectorStyleSheet::~InspectorStyleSheet()
{
    delete m_parsedStyleSheet;
}

bool InspectorStyleSheet::setText(const String& text)
{
    if (!m_parsedStyleSheet)
        return false;

    m_parsedStyleSheet->setText(text);
    for (unsigned i = 0, size = m_pageStyleSheet->length(); i < size; ++i)
        m_pageStyleSheet->remove(i);
    m_inspectorStyles.clear();

    m_pageStyleSheet->parseString(text, m_pageStyleSheet->useStrictParsing());
    return true;
}

bool InspectorStyleSheet::setRuleSelector(const InspectorCSSId& id, const String& selector)
{
    CSSStyleRule* rule = ruleForId(id);
    if (!rule)
        return false;
    CSSStyleSheet* styleSheet = InspectorCSSAgent::parentStyleSheet(rule);
    if (!styleSheet || !ensureParsedDataReady())
        return false;

    rule->setSelectorText(selector);
    RefPtr<CSSRuleSourceData> sourceData = ruleSourceDataFor(rule->style());
    if (!sourceData)
        return false;

    String sheetText = m_parsedStyleSheet->text();
    sheetText.replace(sourceData->selectorListRange.start, sourceData->selectorListRange.end - sourceData->selectorListRange.start, selector);
    m_parsedStyleSheet->setText(sheetText);
    return true;
}

CSSStyleRule* InspectorStyleSheet::addRule(const String& selector)
{
    String styleSheetText;
    bool success = text(&styleSheetText);
    if (!success)
        return 0;

    ExceptionCode ec = 0;
    m_pageStyleSheet->addRule(selector, "", ec);
    if (ec)
        return 0;
    RefPtr<CSSRuleList> rules = m_pageStyleSheet->cssRules();
    ASSERT(rules->length());
    CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(rules->item(rules->length() - 1));
    ASSERT(rule);

    if (styleSheetText.length())
        styleSheetText += "\n";

    styleSheetText += selector;
    styleSheetText += " {}";
    m_parsedStyleSheet->setText(styleSheetText);

    return rule;
}

CSSStyleRule* InspectorStyleSheet::ruleForId(const InspectorCSSId& id) const
{
    if (!m_pageStyleSheet)
        return 0;

    ASSERT(!id.isEmpty());
    bool ok;
    unsigned index = id.ordinal().toUInt(&ok);
    if (!ok)
        return 0;

    unsigned currentIndex = 0;
    for (unsigned i = 0, size = m_pageStyleSheet->length(); i < size; ++i) {
        CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(m_pageStyleSheet->item(i));
        if (!rule)
            continue;
        if (index == currentIndex)
            return rule;

        ++currentIndex;
    }
    return 0;
}

PassRefPtr<InspectorObject> InspectorStyleSheet::buildObjectForStyleSheet()
{
    CSSStyleSheet* styleSheet = pageStyleSheet();
    if (!styleSheet)
        return 0;

    RefPtr<InspectorObject> result = InspectorObject::create();
    result->setBoolean("disabled", styleSheet->disabled());
    result->setString("sourceURL", styleSheet->href());
    result->setString("title", styleSheet->title());
    RefPtr<CSSRuleList> cssRuleList = CSSRuleList::create(styleSheet, true);
    RefPtr<InspectorArray> cssRules = buildArrayForRuleList(cssRuleList.get());
    result->setArray("rules", cssRules.release());

    String styleSheetText;
    bool success = text(&styleSheetText);
    if (success)
        result->setString("text", styleSheetText);

    result->setString("styleSheetId", id());

    return result.release();
}

PassRefPtr<InspectorObject> InspectorStyleSheet::buildObjectForRule(CSSStyleRule* rule)
{
    CSSStyleSheet* styleSheet = pageStyleSheet();
    if (!styleSheet)
        return 0;

    RefPtr<InspectorObject> result = InspectorObject::create();
    result->setString("selectorText", rule->selectorText());
    result->setString("sourceURL", !styleSheet->href().isEmpty() ? styleSheet->href() : m_documentURL);
    result->setNumber("sourceLine", rule->sourceLine());
    result->setString("origin", m_origin);

    result->setObject("style", buildObjectForStyle(rule->style()));
    if (canBind())
        result->setString("ruleId", ruleId(rule).asString());

    return result.release();
}

PassRefPtr<InspectorObject> InspectorStyleSheet::buildObjectForStyle(CSSStyleDeclaration* style)
{
    RefPtr<CSSRuleSourceData> sourceData;
    if (ensureParsedDataReady())
        sourceData = ruleSourceDataFor(style);

    RefPtr<InspectorStyle> inspectorStyle = inspectorStyleForId(ruleOrStyleId(style));
    RefPtr<InspectorObject> result = inspectorStyle->buildObjectForStyle();

    // Style text cannot be retrieved without stylesheet, so set cssText here.
    if (sourceData) {
        String sheetText;
        bool success = text(&sheetText);
        if (success) {
            const SourceRange& bodyRange = sourceData->styleSourceData->styleBodyRange;
            result->setString("cssText", sheetText.substring(bodyRange.start, bodyRange.end - bodyRange.start));
        }
    }

    return result.release();
}

bool InspectorStyleSheet::setPropertyText(const InspectorCSSId& id, unsigned propertyIndex, const String& text, bool overwrite)
{
    RefPtr<InspectorStyle> inspectorStyle = inspectorStyleForId(id);
    if (!inspectorStyle)
        return false;

    return inspectorStyle->setPropertyText(propertyIndex, text, overwrite);
}

bool InspectorStyleSheet::toggleProperty(const InspectorCSSId& id, unsigned propertyIndex, bool disable)
{
    RefPtr<InspectorStyle> inspectorStyle = inspectorStyleForId(id);
    if (!inspectorStyle)
        return false;

    bool success = inspectorStyle->toggleProperty(propertyIndex, disable);
    if (success) {
        if (disable)
            rememberInspectorStyle(inspectorStyle);
        else if (!inspectorStyle->hasDisabledProperties())
            forgetInspectorStyle(inspectorStyle->cssStyle());
    }
    return success;
}

CSSStyleDeclaration* InspectorStyleSheet::styleForId(const InspectorCSSId& id) const
{
    CSSStyleRule* rule = ruleForId(id);
    if (!rule)
        return 0;

    return rule->style();
}

PassRefPtr<InspectorStyle> InspectorStyleSheet::inspectorStyleForId(const InspectorCSSId& id)
{
    CSSStyleDeclaration* style = styleForId(id);
    if (!style)
        return 0;

    InspectorStyleMap::iterator it = m_inspectorStyles.find(style);
    if (it == m_inspectorStyles.end()) {
        RefPtr<InspectorStyle> inspectorStyle = InspectorStyle::create(id, style, this);
        return inspectorStyle.release();
    }
    return it->second;
}

void InspectorStyleSheet::rememberInspectorStyle(RefPtr<InspectorStyle> inspectorStyle)
{
    m_inspectorStyles.set(inspectorStyle->cssStyle(), inspectorStyle);
}

void InspectorStyleSheet::forgetInspectorStyle(CSSStyleDeclaration* style)
{
    m_inspectorStyles.remove(style);
}

InspectorCSSId InspectorStyleSheet::ruleOrStyleId(CSSStyleDeclaration* style) const
{
    unsigned index = ruleIndexByStyle(style);
    if (index != UINT_MAX)
        return InspectorCSSId::createFromParts(id(), String::number(index));
    return InspectorCSSId();
}

Document* InspectorStyleSheet::ownerDocument() const
{
    return m_pageStyleSheet->document();
}

RefPtr<CSSRuleSourceData> InspectorStyleSheet::ruleSourceDataFor(CSSStyleDeclaration* style) const
{
    return m_parsedStyleSheet->ruleSourceDataAt(ruleIndexByStyle(style));
}

unsigned InspectorStyleSheet::ruleIndexByStyle(CSSStyleDeclaration* pageStyle) const
{
    unsigned index = 0;
    for (unsigned i = 0, size = m_pageStyleSheet->length(); i < size; ++i) {
        CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(m_pageStyleSheet->item(i));
        if (!rule)
            continue;
        if (rule->style() == pageStyle)
            return index;

        ++index;
    }
    return UINT_MAX;
}

bool InspectorStyleSheet::ensureParsedDataReady()
{
    return ensureText() && ensureSourceData(pageStyleSheet()->ownerNode());
}

bool InspectorStyleSheet::text(String* result) const
{
    if (!ensureText())
        return false;
    *result = m_parsedStyleSheet->text();
    return true;
}

bool InspectorStyleSheet::ensureText() const
{
    if (!m_parsedStyleSheet)
        return false;
    if (m_parsedStyleSheet->hasText())
        return true;

    String text;
    bool success = originalStyleSheetText(&text);
    if (success)
        m_parsedStyleSheet->setText(text);

    return success;
}

bool InspectorStyleSheet::ensureSourceData(Node* ownerNode)
{
    if (m_parsedStyleSheet->hasSourceData())
        return true;

    if (!m_parsedStyleSheet->hasText())
        return false;

    RefPtr<CSSStyleSheet> newStyleSheet = CSSStyleSheet::create(ownerNode);
    CSSParser p;
    StyleRuleRangeMap ruleRangeMap;
    p.parseSheet(newStyleSheet.get(), m_parsedStyleSheet->text(), 0, &ruleRangeMap);
    OwnPtr<ParsedStyleSheet::SourceData> rangesVector(new ParsedStyleSheet::SourceData());

    for (unsigned i = 0, length = newStyleSheet->length(); i < length; ++i) {
        CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(newStyleSheet->item(i));
        if (!rule)
            continue;
        StyleRuleRangeMap::iterator it = ruleRangeMap.find(rule);
        if (it != ruleRangeMap.end())
            rangesVector->append(it->second);
    }

    m_parsedStyleSheet->setSourceData(rangesVector.release());
    return m_parsedStyleSheet->hasSourceData();
}

bool InspectorStyleSheet::setStyleText(CSSStyleDeclaration* style, const String& text)
{
    if (!pageStyleSheet())
        return false;
    if (!ensureParsedDataReady())
        return false;

    String patchedStyleSheetText;
    bool success = styleSheetTextWithChangedStyle(style, text, &patchedStyleSheetText);
    if (!success)
        return false;

    InspectorCSSId id = ruleOrStyleId(style);
    if (id.isEmpty())
        return false;

    ExceptionCode ec = 0;
    style->setCssText(text, ec);
    if (!ec)
        m_parsedStyleSheet->setText(patchedStyleSheetText);

    return !ec;
}

bool InspectorStyleSheet::styleSheetTextWithChangedStyle(CSSStyleDeclaration* style, const String& newStyleText, String* result)
{
    if (!style)
        return false;

    if (!ensureParsedDataReady())
        return false;

    RefPtr<CSSRuleSourceData> sourceData = ruleSourceDataFor(style);
    unsigned bodyStart = sourceData->styleSourceData->styleBodyRange.start;
    unsigned bodyEnd = sourceData->styleSourceData->styleBodyRange.end;
    ASSERT(bodyStart <= bodyEnd);

    String text = m_parsedStyleSheet->text();
    ASSERT(bodyEnd <= text.length()); // bodyEnd is exclusive

    text.replace(bodyStart, bodyEnd - bodyStart, newStyleText);
    *result = text;
    return true;
}

CSSStyleRule* InspectorStyleSheet::findPageRuleWithStyle(CSSStyleDeclaration* style)
{
    for (unsigned i = 0, size = m_pageStyleSheet->length(); i < size; ++i) {
        CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(m_pageStyleSheet->item(i));
        if (!rule)
            continue;
        if (rule->style() == style)
            return rule;
    }
    return 0;
}

InspectorCSSId InspectorStyleSheet::ruleId(CSSStyleRule* rule) const
{
    return ruleOrStyleId(rule->style());
}

void InspectorStyleSheet::revalidateStyle(CSSStyleDeclaration* pageStyle)
{
    if (m_isRevalidating)
        return;

    m_isRevalidating = true;
    CSSStyleSheet* parsedSheet = m_parsedStyleSheet->cssStyleSheet();
    for (unsigned i = 0, size = parsedSheet->length(); i < size; ++i) {
        StyleBase* styleBase = parsedSheet->item(i);
        CSSStyleRule* parsedRule = InspectorCSSAgent::asCSSStyleRule(styleBase);
        if (!parsedRule)
            continue;
        if (parsedRule->style() == pageStyle) {
            if (parsedRule->style()->cssText() != pageStyle->cssText()) {
                // Clear the disabled properties for the invalid style here.
                m_inspectorStyles.remove(pageStyle);
                setStyleText(pageStyle, pageStyle->cssText());
            }
            break;
        }
    }
    m_isRevalidating = false;
}

bool InspectorStyleSheet::originalStyleSheetText(String* result) const
{
    String text;
    bool success = inlineStyleSheetText(&text);
    if (!success)
        success = resourceStyleSheetText(&text);
    if (success)
        *result = text;
    return success;
}

bool InspectorStyleSheet::resourceStyleSheetText(String* result) const
{
    if (!m_pageStyleSheet || !ownerDocument())
        return false;

    return InspectorResourceAgent::resourceContent(ownerDocument()->frame(), m_pageStyleSheet->finalURL(), result);
}

bool InspectorStyleSheet::inlineStyleSheetText(String* result) const
{
    if (!m_pageStyleSheet)
        return false;

    Node* ownerNode = m_pageStyleSheet->ownerNode();
    if (!ownerNode || ownerNode->nodeType() != Node::ELEMENT_NODE)
        return false;
    Element* ownerElement = static_cast<Element*>(ownerNode);
    if (ownerElement->tagName().lower() != "style")
        return false;
    *result = ownerElement->innerText();
    return true;
}

PassRefPtr<InspectorArray> InspectorStyleSheet::buildArrayForRuleList(CSSRuleList* ruleList)
{
    RefPtr<InspectorArray> result = InspectorArray::create();
    if (!ruleList)
        return result.release();

    for (unsigned i = 0, size = ruleList->length(); i < size; ++i) {
        CSSStyleRule* rule = InspectorCSSAgent::asCSSStyleRule(ruleList->item(i));
        if (!rule)
            continue;

        result->pushObject(buildObjectForRule(rule));
    }
    return result.release();
}


InspectorStyleSheetForInlineStyle::InspectorStyleSheetForInlineStyle(const String& id, Element* element, const String& origin)
    : InspectorStyleSheet(id, 0, origin, "")
    , m_element(element)
    , m_ruleSourceData(0)
{
    ASSERT(element);
    m_inspectorStyle = InspectorStyle::create(InspectorCSSId::createFromParts(id, "0"), inlineStyle(), this);
}

bool InspectorStyleSheetForInlineStyle::setStyleText(CSSStyleDeclaration* style, const String& text)
{
    ASSERT_UNUSED(style, style == inlineStyle());
    ExceptionCode ec = 0;
    m_element->setAttribute("style", text, ec);
    m_ruleSourceData.clear();
    return !ec;
}

Document* InspectorStyleSheetForInlineStyle::ownerDocument() const
{
    return m_element->document();
}

bool InspectorStyleSheetForInlineStyle::ensureParsedDataReady()
{
    if (m_ruleSourceData)
        return true;

    m_ruleSourceData = CSSRuleSourceData::create();
    RefPtr<CSSStyleSourceData> sourceData = CSSStyleSourceData::create();
    bool success = getStyleAttributeRanges(&sourceData);
    if (!success)
        return false;

    m_ruleSourceData->styleSourceData = sourceData.release();
    return true;
}

PassRefPtr<InspectorStyle> InspectorStyleSheetForInlineStyle::inspectorStyleForId(const InspectorCSSId& id)
{
    ASSERT_UNUSED(id, id.ordinal() == "0");
    return m_inspectorStyle;
}

CSSStyleDeclaration* InspectorStyleSheetForInlineStyle::inlineStyle() const
{
    return m_element->style();
}

bool InspectorStyleSheetForInlineStyle::getStyleAttributeRanges(RefPtr<CSSStyleSourceData>* result)
{
    DEFINE_STATIC_LOCAL(String, styleAttributeName, ("style"));

    if (!m_element->isStyledElement())
        return false;

    String styleText = static_cast<StyledElement*>(m_element)->getAttribute(styleAttributeName);
    if (styleText.isEmpty()) {
        (*result)->styleBodyRange.start = 0;
        (*result)->styleBodyRange.end = 0;
        return true;
    }

    RefPtr<CSSMutableStyleDeclaration> tempDeclaration = CSSMutableStyleDeclaration::create();
    CSSParser p;
    p.parseDeclaration(tempDeclaration.get(), styleText, result);
    return true;
}

} // namespace WebCore

#endif // ENABLE(INSPECTOR)