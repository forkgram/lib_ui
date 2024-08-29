// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text_word_parser.h"

#include "ui/text/text_bidi_algorithm.h"
#include "styles/style_basic.h"

// COPIED FROM qtextlayout.cpp AND MODIFIED
namespace Ui::Text {
namespace {

struct ScriptLine {
	int length = 0;
	QFixed textWidth;
};

// All members finished with "_" are internal.
struct LineBreakHelper {
	ScriptLine tmpData;
	ScriptLine spaceData;

	QGlyphLayout glyphs;

	int glyphCount = 0;
	int maxGlyphs = INT_MAX;
	int currentPosition = 0;

	glyph_t previousGlyph = 0;
	QExplicitlySharedDataPointer<QFontEngine> previousGlyphFontEngine;

	QFixed rightBearing;

	QExplicitlySharedDataPointer<QFontEngine> fontEngine;
	const unsigned short *logClusters = nullptr;

	bool whiteSpaceOrObject = true;

	glyph_t currentGlyph() const;
	void saveCurrentGlyph();
	void calculateRightBearing(QFontEngine *engine, glyph_t glyph);
	void calculateRightBearing();
	void calculateRightBearingForPreviousGlyph();

	// We always calculate the right bearing right before it is needed.
	// So we don't need caching / optimizations referred to
	// delayed right bearing calculations.

	//static const QFixed RightBearingNotCalculated;

	//inline void resetRightBearing()
	//{
	//	rightBearing = RightBearingNotCalculated;
	//}

	// We express the negative right bearing as an absolute number
	// so that it can be applied to the width using addition.
	QFixed negativeRightBearing() const;

};

//const QFixed LineBreakHelper::RightBearingNotCalculated = QFixed(1);

glyph_t LineBreakHelper::currentGlyph() const {
	Q_ASSERT(currentPosition > 0);
	Q_ASSERT(logClusters[currentPosition - 1] < glyphs.numGlyphs);

	return glyphs.glyphs[logClusters[currentPosition - 1]];
}

void LineBreakHelper::saveCurrentGlyph() {
	if (currentPosition > 0
		&& logClusters[currentPosition - 1] < glyphs.numGlyphs) {
		// needed to calculate right bearing later
		previousGlyph = currentGlyph();
		previousGlyphFontEngine = fontEngine;
	} else {
		previousGlyph = 0;
		previousGlyphFontEngine = nullptr;
	}
}

void LineBreakHelper::calculateRightBearing(
		QFontEngine *engine,
		glyph_t glyph) {
	qreal rb;
	engine->getGlyphBearings(glyph, 0, &rb);

	// We only care about negative right bearings, so we limit the range
	// of the bearing here so that we can assume it's negative in the rest
	// of the code, as well as use QFixed(1) as a sentinel to represent
	// the state where we have yet to compute the right bearing.
	rightBearing = qMin(QFixed::fromReal(rb), QFixed(0));
}

void LineBreakHelper::calculateRightBearing() {
	if (currentPosition > 0
		&& logClusters[currentPosition - 1] < glyphs.numGlyphs
		&& !whiteSpaceOrObject) {
		calculateRightBearing(fontEngine.data(), currentGlyph());
	} else {
		rightBearing = 0;
	}
}

void LineBreakHelper::calculateRightBearingForPreviousGlyph() {
	if (previousGlyph > 0) {
		calculateRightBearing(previousGlyphFontEngine.data(), previousGlyph);
	} else {
		rightBearing = 0;
	}
}

// We always calculate the right bearing right before it is needed.
// So we don't need caching / optimizations referred to delayed right bearing calculations.

//static const QFixed RightBearingNotCalculated;

//inline void resetRightBearing()
//{
//	rightBearing = RightBearingNotCalculated;
//}

// We express the negative right bearing as an absolute number
// so that it can be applied to the width using addition.
QFixed LineBreakHelper::negativeRightBearing() const {
	//if (rightBearing == RightBearingNotCalculated)
	//	return QFixed(0);

	return qAbs(rightBearing);
}

void addNextCluster(
		int &pos,
		int end,
		ScriptLine &line,
		int &glyphCount,
		const QScriptItem &current,
		const unsigned short *logClusters,
		const QGlyphLayout &glyphs) {
	int glyphPosition = logClusters[pos];
	do { // got to the first next cluster
		++pos;
		++line.length;
	} while (pos < end && logClusters[pos] == glyphPosition);
	do { // calculate the textWidth for the rest of the current cluster.
		if (!glyphs.attributes[glyphPosition].dontPrint)
			line.textWidth += glyphs.advances[glyphPosition];
		++glyphPosition;
	} while (glyphPosition < current.num_glyphs
		&& !glyphs.attributes[glyphPosition].clusterStart);

	Q_ASSERT((pos == end && glyphPosition == current.num_glyphs)
		|| logClusters[pos] == glyphPosition);

	++glyphCount;
}

} // anonymous namespace

WordParser::BidiInitedAnalysis::BidiInitedAnalysis(not_null<String*> text)
: list(text->_text.size()) {
	BidiAlgorithm bidi(
		text->_text.constData(),
		list.data(),
		text->_text.size(),
		false, // baseDirectionIsRtl
		begin(text->_blocks),
		end(text->_blocks),
		0); // offsetInBlocks
	bidi.process();
}

WordParser::WordParser(not_null<String*> string)
: _t(string)
, _tText(_t->_text)
, _tBlocks(_t->_blocks)
, _tWords(_t->_words)
, _analysis(_t)
, _engine(_t, _analysis.list) {
	parse();
}

void WordParser::parse() {
	_tWords.clear();
	if (_tText.isEmpty()) {
		return;
	}
	auto &e = _engine.wrapped();

	LineBreakHelper lbh;

	int item = -1;
	int newItem = e.findItem(0);

	const QCharAttributes *attributes = e.attributes();
	if (!attributes)
		return;
	int end = 0;
	lbh.logClusters = e.layoutData->logClustersPtr;

	int wordStart = lbh.currentPosition;

	bool addingEachGrapheme = false;
	int lastGraphemeBoundaryPosition = -1;
	ScriptLine lastGraphemeBoundaryLine;

	while (newItem < e.layoutData->items.size()) {
		if (newItem != item) {
			item = newItem;
			auto &si = e.layoutData->items[item];
			if (!si.num_glyphs) {
				_engine.shapeGetBlock(item);
				attributes = e.attributes();
				if (!attributes)
					return;
				lbh.logClusters = e.layoutData->logClustersPtr;
			}
			lbh.currentPosition = si.position;
			end = si.position + e.length(item);
			lbh.glyphs = e.shapedGlyphs(&si);
			QFontEngine *fontEngine = e.fontEngine(si);
			if (lbh.fontEngine != fontEngine) {
				lbh.fontEngine = fontEngine;
			}
		}
		const QScriptItem &current = e.layoutData->items[item];
		const auto atSpaceBreak = [&] {
			for (auto index = lbh.currentPosition; index < end; ++index) {
				if (!attributes[index].whiteSpace) {
					return false;
				} else if (isSpaceBreak(attributes, index)) {
					return true;
				}
			}
			return false;
		}();
		if (current.analysis.flags == QScriptAnalysis::LineOrParagraphSeparator) {
			if (wordStart < lbh.currentPosition) {
				lbh.calculateRightBearing();
				pushFinishedWord(
					wordStart,
					lbh.tmpData.textWidth,
					-lbh.negativeRightBearing());
				lbh.tmpData.textWidth = 0;
				lbh.tmpData.length = 0;
				wordStart = lbh.currentPosition;

				addingEachGrapheme = false;
				lastGraphemeBoundaryPosition = -1;
				lastGraphemeBoundaryLine = ScriptLine();
			}

			lbh.whiteSpaceOrObject = true;
			lbh.tmpData.length++;

			newItem = item + 1;
			++lbh.glyphCount;

			pushNewline(wordStart, _engine.blockIndex(wordStart));
			lbh.tmpData.textWidth = 0;
			lbh.tmpData.length = 0;
			wordStart = end;

			addingEachGrapheme = false;
			lastGraphemeBoundaryPosition = -1;
			lastGraphemeBoundaryLine = ScriptLine();
		} else if (current.analysis.flags == QScriptAnalysis::Object) {
			if (wordStart < lbh.currentPosition) {
				lbh.calculateRightBearing();
				pushFinishedWord(
					wordStart,
					lbh.tmpData.textWidth,
					-lbh.negativeRightBearing());
				lbh.tmpData.textWidth = 0;
				lbh.tmpData.length = 0;
				wordStart = lbh.currentPosition;

				addingEachGrapheme = false;
				lastGraphemeBoundaryPosition = -1;
				lastGraphemeBoundaryLine = ScriptLine();
			}

			lbh.whiteSpaceOrObject = true;
			lbh.tmpData.length++;
			lbh.tmpData.textWidth += current.width;

			newItem = item + 1;
			++lbh.glyphCount;

			lbh.calculateRightBearing();
			pushFinishedWord(
				wordStart,
				lbh.tmpData.textWidth,
				-lbh.negativeRightBearing());
			lbh.tmpData.textWidth = 0;
			lbh.tmpData.length = 0;
			wordStart = end;

			addingEachGrapheme = false;
			lastGraphemeBoundaryPosition = -1;
			lastGraphemeBoundaryLine = ScriptLine();
		} else if (atSpaceBreak) {
			lbh.whiteSpaceOrObject = true;
			while (lbh.currentPosition < end
				&& attributes[lbh.currentPosition].whiteSpace)
				addNextCluster(
					lbh.currentPosition,
					end,
					lbh.spaceData,
					lbh.glyphCount,
					current,
					lbh.logClusters,
					lbh.glyphs);

			if (_tWords.empty()) {
				lbh.calculateRightBearing();
				pushFinishedWord(
					wordStart,
					lbh.tmpData.textWidth,
					-lbh.negativeRightBearing());
			}
			_tWords.back().add_rpadding(lbh.spaceData.textWidth);
			lbh.spaceData.length = 0;
			lbh.spaceData.textWidth = 0;

			wordStart = lbh.currentPosition;

			addingEachGrapheme = false;
			lastGraphemeBoundaryPosition = -1;
			lastGraphemeBoundaryLine = ScriptLine();
		} else {
			lbh.whiteSpaceOrObject = false;
			do {
				addNextCluster(
					lbh.currentPosition,
					end,
					lbh.tmpData,
					lbh.glyphCount,
					current,
					lbh.logClusters,
					lbh.glyphs);

				if (lbh.currentPosition >= e.layoutData->string.length()
					|| isSpaceBreak(attributes, lbh.currentPosition)
					|| isLineBreak(attributes, lbh.currentPosition)) {
					lbh.calculateRightBearing();
					pushFinishedWord(
						wordStart,
						lbh.tmpData.textWidth,
						-lbh.negativeRightBearing());
					lbh.tmpData.textWidth = 0;
					lbh.tmpData.length = 0;
					wordStart = lbh.currentPosition;

					addingEachGrapheme = false;
					lastGraphemeBoundaryPosition = -1;
					lastGraphemeBoundaryLine = ScriptLine();
					break;
				} else if (attributes[lbh.currentPosition].graphemeBoundary) {
					if (!addingEachGrapheme && lbh.tmpData.textWidth > _t->_minResizeWidth) {
						if (lastGraphemeBoundaryPosition >= 0) {
							lbh.calculateRightBearingForPreviousGlyph();
							pushUnfinishedWord(
								wordStart,
								lastGraphemeBoundaryLine.textWidth,
								-lbh.negativeRightBearing());
							lbh.tmpData.textWidth -= lastGraphemeBoundaryLine.textWidth;
							lbh.tmpData.length -= lastGraphemeBoundaryLine.length;
							wordStart = lastGraphemeBoundaryPosition;
						}
						addingEachGrapheme = true;
					}
					if (addingEachGrapheme) {
						lbh.calculateRightBearing();
						pushUnfinishedWord(
							wordStart,
							lbh.tmpData.textWidth,
							-lbh.negativeRightBearing());
						lbh.tmpData.textWidth = 0;
						lbh.tmpData.length = 0;
						wordStart = lbh.currentPosition;
					} else {
						lastGraphemeBoundaryPosition = lbh.currentPosition;
						lastGraphemeBoundaryLine = lbh.tmpData;
						lbh.saveCurrentGlyph();
					}
				}
			} while (lbh.currentPosition < end);
		}
		if (lbh.currentPosition == end)
			newItem = item + 1;
	}
	if (!_tWords.empty()) {
		_tWords.shrink_to_fit();
	}
}

void WordParser::pushFinishedWord(
		uint16 position,
		QFixed width,
		QFixed rbearing) {
	const auto unfinished = false;
	_tWords.push_back(Word(position, unfinished, width, rbearing));
}

void WordParser::pushUnfinishedWord(
		uint16 position,
		QFixed width,
		QFixed rbearing) {
	const auto unfinished = true;
	_tWords.push_back(Word(position, unfinished, width, rbearing));
}

void WordParser::pushNewline(uint16 position, int newlineBlockIndex) {
	_tWords.push_back(Word(position, newlineBlockIndex));
}

bool WordParser::isLineBreak(
		const QCharAttributes *attributes,
		int index) const {
	// Don't break by '/' or '.' in the middle of the word.
	// In case of a line break or white space it'll allow break anyway.
	return attributes[index].lineBreak
		&& (index <= 0
			|| (_tText[index - 1] != '/' && _tText[index - 1] != '.'));
}

bool WordParser::isSpaceBreak(
		const QCharAttributes *attributes,
		int index) const {
	// Don't break on &nbsp;
	return attributes[index].whiteSpace && (_tText[index] != QChar::Nbsp);
}

} // namespace Ui::Text
