// Scintilla source code edit control
/** @file LexDart.cxx
 ** Lexer for Dart.
 **/
// Based on Zufu Liu's Notepad4 Dart lexer
// Modified for Scintilla by Jiri Techet, 2024
// The License.txt file describes the conditions under which this software may be distributed.

#include <cassert>
#include <cstring>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"

using namespace Lexilla;

namespace {
// Use an unnamed namespace to protect the functions and classes from name conflicts

constexpr bool IsEOLChar(int ch) noexcept {
	return ch == '\r' || ch == '\n';
}

constexpr bool IsAGraphic(int ch) noexcept {
	// excludes C0 control characters and whitespace
	return ch > 32 && ch < 127;
}

constexpr bool IsIdentifierChar(int ch) noexcept {
	return IsAlphaNumeric(ch) || ch == '_';
}

constexpr bool IsIdentifierStart(int ch) noexcept {
	return IsUpperOrLowerCase(ch) || ch == '_';
}

constexpr bool IsNumberContinue(int chPrev, int ch, int chNext) noexcept {
	return ((ch == '+' || ch == '-') && (chPrev == 'e' || chPrev == 'E'))
		|| (ch == '.' && chNext != '.');
}

constexpr bool IsNumberStart(int ch, int chNext) noexcept {
	return IsADigit(ch) || (ch == '.' && IsADigit(chNext));
}

constexpr bool IsDecimalNumber(int chPrev, int ch, int chNext) noexcept {
	return IsIdentifierChar(ch) || IsNumberContinue(chPrev, ch, chNext);
}

struct EscapeSequence {
	int outerState = SCE_DART_DEFAULT;
	int digitsLeft = 0;
	bool brace = false;

	// highlight any character as escape sequence.
	bool resetEscapeState(int state, int chNext) noexcept {
		if (IsEOLChar(chNext)) {
			return false;
		}
		outerState = state;
		brace = false;
		digitsLeft = (chNext == 'x')? 3 : ((chNext == 'u') ? 5 : 1);
		return true;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsAHeXDigit(ch);
	}
};

enum {
	DartLineStateMaskLineComment = 1,			// line comment
	DartLineStateMaskImport = (1 << 1),			// import
	DartLineStateMaskInterpolation = (1 << 2),	// string interpolation
};

enum {
	KeywordIndex_Primary = 0,
	KeywordIndex_Secondary = 1,
	KeywordIndex_Tertiary = 2,
	KeywordIndex_Type = 3,
};

const char *const dartWordListDesc[] = {
	"Primary keywords",
	"Secondary keywords",
	"Tertiary keywords",
	"Global type definitions",
	nullptr
};

constexpr bool IsDartIdentifierStart(int ch) noexcept {
	return IsIdentifierStart(ch) || ch == '$';
}

constexpr bool IsDartIdentifierChar(int ch) noexcept {
	return IsIdentifierChar(ch) || ch == '$';
}

constexpr bool IsDefinableOperator(int ch) noexcept {
	// https://github.com/dart-lang/sdk/blob/main/sdk/lib/core/symbol.dart
	return AnyOf(ch, '+', '-', '*', '/', '%', '~', '&', '|',
					 '^', '<', '>', '=', '[', ']');
}

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state == SCE_DART_DEFAULT ||
		state == SCE_DART_COMMENTLINE ||
		state == SCE_DART_COMMENTLINEDOC ||
		state == SCE_DART_COMMENTBLOCK ||
		state == SCE_DART_COMMENTBLOCKDOC;
}

constexpr bool IsTripleString(int state) noexcept {
	return state == SCE_DART_TRIPLE_STRING_SQ ||
		state == SCE_DART_TRIPLE_STRING_DQ ||
		state == SCE_DART_TRIPLE_RAWSTRING_SQ ||
		state == SCE_DART_TRIPLE_RAWSTRING_DQ;
}

constexpr bool IsDoubleQuoted(int state) noexcept {
	return state == SCE_DART_STRING_DQ ||
		state == SCE_DART_RAWSTRING_DQ ||
		state == SCE_DART_TRIPLE_STRING_DQ ||
		state == SCE_DART_TRIPLE_RAWSTRING_DQ;
}

constexpr bool IsRaw(int state) noexcept {
	return state == SCE_DART_RAWSTRING_SQ ||
		state == SCE_DART_RAWSTRING_DQ ||
		state == SCE_DART_TRIPLE_RAWSTRING_SQ ||
		state == SCE_DART_TRIPLE_RAWSTRING_DQ;
}

constexpr int GetStringQuote(int state) noexcept {
	return IsDoubleQuoted(state) ? '\"' : '\'';
}

// string interpolating state
struct InterpolatingState {
	int state;
	int braceCount;
};

void BacktrackToStart(const LexAccessor &styler, int stateMask, Sci_PositionU &startPos, Sci_Position &lengthDoc, int &initStyle) {
	const Sci_Position currentLine = styler.GetLine(startPos);
	if (currentLine != 0) {
		Sci_Position line = currentLine - 1;
		int lineState = styler.GetLineState(line);
		while ((lineState & stateMask) != 0 && line != 0) {
			--line;
			lineState = styler.GetLineState(line);
		}
		if ((lineState & stateMask) == 0) {
			++line;
		}
		if (line != currentLine) {
			const Sci_PositionU endPos = startPos + lengthDoc;
			startPos = (line == 0) ? 0 : styler.LineStart(line);
			lengthDoc = endPos - startPos;
			initStyle = (startPos == 0) ? 0 : styler.StyleAt(startPos - 1);
		}
	}
}

Sci_PositionU LookbackNonWhite(LexAccessor &styler, Sci_PositionU startPos, int &chPrevNonWhite, int &stylePrevNonWhite) {
	do {
		--startPos;
		const unsigned style = styler.StyleAt(startPos);
		if (!IsSpaceEquiv(style)) {
			stylePrevNonWhite = style;
			chPrevNonWhite = static_cast<unsigned char>(styler[startPos]);
			break;
		}
	} while (startPos != 0);
	return startPos;
}

void ColouriseDartDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, WordList *keywordLists[], Accessor &styler) {
	int lineStateLineType = 0;
	int commentLevel = 0;	// nested block comment level

	std::vector<InterpolatingState> interpolatingStack;

	int visibleChars = 0;
	int chBefore = 0;
	int chPrevNonWhite = 0;
	EscapeSequence escSeq;

	if (startPos != 0) {
		// backtrack to the line where interpolation starts
		BacktrackToStart(styler, DartLineStateMaskInterpolation, startPos, lengthDoc, initStyle);
	}

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		const int lineState = styler.GetLineState(sc.currentLine - 1);
		commentLevel = lineState >> 4;
	}
	if (startPos == 0) {
		if (sc.Match('#', '!')) {
			// Shell Shebang at beginning of file
			sc.SetState(SCE_DART_COMMENTLINE);
			sc.Forward();
			lineStateLineType = DartLineStateMaskLineComment;
		}
	} else if (IsSpaceEquiv(initStyle)) {
		LookbackNonWhite(styler, startPos, chPrevNonWhite, initStyle);
		chBefore = chPrevNonWhite;
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_DART_OPERATOR:
		case SCE_DART_OPERATOR_STRING:
			sc.SetState(SCE_DART_DEFAULT);
			break;

		case SCE_DART_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_DART_DEFAULT);
			}
			break;

		case SCE_DART_IDENTIFIER:
		case SCE_DART_IDENTIFIER_STRING:
		case SCE_DART_METADATA:
		case SCE_DART_SYMBOL_IDENTIFIER:
			if (!IsDartIdentifierChar(sc.ch) || (sc.ch == '$' && sc.state == SCE_DART_IDENTIFIER_STRING)) {
				if (sc.state == SCE_DART_METADATA || sc.state == SCE_DART_SYMBOL_IDENTIFIER) {
					if (sc.ch == '.') {
						const int state = sc.state;
						sc.SetState(SCE_DART_OPERATOR);
						sc.ForwardSetState(state);
						continue;
					}
				} else {
					char s[64];
					sc.GetCurrent(s, sizeof(s));
					const int state = sc.state;
					if (state == SCE_DART_IDENTIFIER_STRING) {
						sc.SetState(escSeq.outerState);
						continue;
					} else if (keywordLists[KeywordIndex_Primary]->InList(s)) {
						sc.ChangeState(SCE_DART_KW_PRIMARY);
						if (strcmp(s, "import") == 0 || strcmp(s, "part") == 0) {
							if (visibleChars == sc.LengthCurrent()) {
								lineStateLineType = DartLineStateMaskImport;
							}
						}
					} else if (keywordLists[KeywordIndex_Secondary]->InList(s)) {
						sc.ChangeState(SCE_DART_KW_SECONDARY);
					} else if (keywordLists[KeywordIndex_Tertiary]->InList(s)) {
						sc.ChangeState(SCE_DART_KW_TERTIARY);
					} else if (keywordLists[KeywordIndex_Type]->InList(s)) {
						sc.ChangeState(SCE_DART_KW_TYPE);
					} else if (state == SCE_DART_IDENTIFIER && sc.ch == ':') {
						if (chBefore == ',' || chBefore == '{' || chBefore == '(') {
							sc.ChangeState(SCE_DART_KEY); // map key or named parameter
						}
					}
				}

				sc.SetState(SCE_DART_DEFAULT);
			}
			break;

		case SCE_DART_SYMBOL_OPERATOR:
			if (!IsDefinableOperator(sc.ch)) {
				sc.SetState(SCE_DART_DEFAULT);
			}
			break;

		case SCE_DART_COMMENTLINE:
		case SCE_DART_COMMENTLINEDOC:
			if (sc.atLineStart) {
				sc.SetState(SCE_DART_DEFAULT);
			}
			break;

		case SCE_DART_COMMENTBLOCK:
		case SCE_DART_COMMENTBLOCKDOC:
			if (sc.Match('*', '/')) {
				sc.Forward();
				--commentLevel;
				if (commentLevel == 0) {
					sc.ForwardSetState(SCE_DART_DEFAULT);
				}
			} else if (sc.Match('/', '*')) {
				sc.Forward();
				++commentLevel;
			}
			break;

		case SCE_DART_STRING_SQ:
		case SCE_DART_STRING_DQ:
		case SCE_DART_TRIPLE_STRING_SQ:
		case SCE_DART_TRIPLE_STRING_DQ:
		case SCE_DART_RAWSTRING_SQ:
		case SCE_DART_RAWSTRING_DQ:
		case SCE_DART_TRIPLE_RAWSTRING_SQ:
		case SCE_DART_TRIPLE_RAWSTRING_DQ:
			if (sc.atLineStart && !IsTripleString(sc.state)) {
				sc.SetState(SCE_DART_DEFAULT);
			} else if (sc.ch == '\\' && !IsRaw(sc.state)) {
				if (escSeq.resetEscapeState(sc.state, sc.chNext)) {
					sc.SetState(SCE_DART_ESCAPECHAR);
					sc.Forward();
					if (sc.Match('u', '{')) {
						escSeq.brace = true;
						escSeq.digitsLeft = 7; // Unicode code point
						sc.Forward();
					}
				}
			} else if (sc.ch == '$' && !IsRaw(sc.state)) {
				escSeq.outerState = sc.state;
				sc.SetState(SCE_DART_OPERATOR_STRING);
				sc.Forward();
				if (sc.ch == '{') {
					interpolatingStack.push_back({escSeq.outerState, 1});
				} else if (sc.ch != '$' && IsDartIdentifierStart(sc.ch)) {
					sc.SetState(SCE_DART_IDENTIFIER_STRING);
				} else { // error
					sc.SetState(escSeq.outerState);
					continue;
				}
			} else if (sc.ch == GetStringQuote(sc.state) &&
					(!IsTripleString(sc.state) || (sc.Match(IsDoubleQuoted(sc.state) ? R"(""")" : R"(''')")))) {
				if (IsTripleString(sc.state)) {
					sc.Forward(2);
				}
				sc.Forward();
				sc.SetState(SCE_DART_DEFAULT);
			}
			break;

		case SCE_DART_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				if (escSeq.brace && sc.ch == '}') {
					sc.Forward();
				}
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;
		}

		if (sc.state == SCE_DART_DEFAULT) {
			if (sc.ch == '/' && (sc.chNext == '/' || sc.chNext == '*')) {
				const int chNext = sc.chNext;
				sc.SetState((chNext == '/') ? SCE_DART_COMMENTLINE : SCE_DART_COMMENTBLOCK);
				sc.Forward(2);
				if (sc.ch == chNext && sc.chNext != chNext) {
					if (sc.state == SCE_DART_COMMENTLINE) {
						sc.ChangeState(SCE_DART_COMMENTLINEDOC);
					} else {
						sc.ChangeState(SCE_DART_COMMENTBLOCKDOC);
					}
				}
				if (chNext == '/') {
					if (visibleChars == 0) {
						lineStateLineType = DartLineStateMaskLineComment;
					}
				 } else {
					commentLevel = 1;
				 }
				 continue;
			}
			if (sc.ch == 'r' && (sc.chNext == '\'' || sc.chNext == '"')) {
				sc.SetState((sc.chNext == '\'') ? SCE_DART_RAWSTRING_SQ : SCE_DART_RAWSTRING_DQ);
				sc.Forward(2);
				if (sc.chPrev == '\'' && sc.Match('\'', '\'')) {
					sc.ChangeState(SCE_DART_TRIPLE_RAWSTRING_SQ);
					sc.Forward(2);
				} else if (sc.chPrev == '"' && sc.Match('"', '"')) {
					sc.ChangeState(SCE_DART_TRIPLE_RAWSTRING_DQ);
					sc.Forward(2);
				}
				continue;
			}
			if (sc.ch == '"') {
				if (sc.Match(R"(""")")) {
					sc.SetState(SCE_DART_TRIPLE_STRING_DQ);
					sc.Forward(2);
				} else {
					chBefore = chPrevNonWhite;
					sc.SetState(SCE_DART_STRING_DQ);
				}
			} else if (sc.ch == '\'') {
				if (sc.Match(R"(''')")) {
					sc.SetState(SCE_DART_TRIPLE_STRING_SQ);
					sc.Forward(2);
				} else {
					chBefore = chPrevNonWhite;
					sc.SetState(SCE_DART_STRING_SQ);
				}
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_DART_NUMBER);
			} else if ((sc.ch == '@' || sc.ch == '#') && IsDartIdentifierStart(sc.chNext)) {
				sc.SetState((sc.ch == '@') ? SCE_DART_METADATA : SCE_DART_SYMBOL_IDENTIFIER);
			} else if (IsDartIdentifierStart(sc.ch)) {
				chBefore = chPrevNonWhite;
				sc.SetState(SCE_DART_IDENTIFIER);
			} else if (sc.ch == '#' && IsDefinableOperator(sc.chNext)) {
				sc.SetState(SCE_DART_SYMBOL_OPERATOR);
			} else if (IsAGraphic(sc.ch)) {
				sc.SetState(SCE_DART_OPERATOR);
				if (!interpolatingStack.empty() && AnyOf(sc.ch, '{', '}')) {
					InterpolatingState &current = interpolatingStack.back();
					if (sc.ch == '{') {
						current.braceCount += 1;
					} else {
						current.braceCount -= 1;
						if (current.braceCount == 0) {
							sc.ChangeState(SCE_DART_OPERATOR_STRING);
							sc.ForwardSetState(current.state);
							interpolatingStack.pop_back();
							continue;
						}
					}
				}
			}
		}

		if (!isspacechar(sc.ch)) {
			visibleChars++;
			if (!IsSpaceEquiv(sc.state)) {
				chPrevNonWhite = sc.ch;
			}
		}
		if (sc.atLineEnd) {
			int lineState = (commentLevel << 4) | lineStateLineType;
			if (!interpolatingStack.empty()) {
				lineState |= DartLineStateMaskInterpolation;
			}
			styler.SetLineState(sc.currentLine, lineState);
			lineStateLineType = 0;
			visibleChars = 0;
		}
		sc.Forward();
	}

	sc.Complete();
}

struct FoldLineState {
	int lineComment;
	int packageImport;
	constexpr explicit FoldLineState(int lineState) noexcept:
		lineComment(lineState & DartLineStateMaskLineComment),
		packageImport((lineState >> 1) & 1) {
	}
};

void FoldDartDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, WordList *[] /*keywordLists*/, Accessor &styler) {
	const Sci_PositionU endPos = startPos + lengthDoc;
	Sci_Position lineCurrent = styler.GetLine(startPos);
	FoldLineState foldPrev(0);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
		foldPrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
	}

	int levelNext = levelCurrent;
	FoldLineState foldCurrent(styler.GetLineState(lineCurrent));
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = std::min(lineStartNext, endPos);

	char chNext = styler[startPos];
	int styleNext = styler.StyleIndexAt(startPos);
	int style = initStyle;

	while (startPos < endPos) {
		const char ch = chNext;
		const int stylePrev = style;
		style = styleNext;
		chNext = styler[++startPos];
		styleNext = styler.StyleIndexAt(startPos);

		switch (style) {
		case SCE_DART_COMMENTBLOCKDOC:
		case SCE_DART_COMMENTBLOCK: {
			const int level = (ch == '/' && chNext == '*') ? 1 : ((ch == '*' && chNext == '/') ? -1 : 0);
			if (level != 0) {
				levelNext += level;
				startPos++;
				chNext = styler[startPos];
				styleNext = styler.StyleIndexAt(startPos);
			}
		} break;

		case SCE_DART_TRIPLE_RAWSTRING_SQ:
		case SCE_DART_TRIPLE_RAWSTRING_DQ:
		case SCE_DART_TRIPLE_STRING_SQ:
		case SCE_DART_TRIPLE_STRING_DQ:
			if (style != stylePrev && !AnyOf(stylePrev, SCE_DART_ESCAPECHAR, SCE_DART_OPERATOR_STRING, SCE_DART_IDENTIFIER_STRING)) {
				levelNext++;
			}
			if (style != styleNext && !AnyOf(styleNext, SCE_DART_ESCAPECHAR, SCE_DART_OPERATOR_STRING, SCE_DART_IDENTIFIER_STRING)) {
				levelNext--;
			}
			break;

		case SCE_DART_OPERATOR:
		case SCE_DART_OPERATOR_STRING:
			if (ch == '{' || ch == '[' || ch == '(') {
				levelNext++;
			} else if (ch == '}' || ch == ']' || ch == ')') {
				levelNext--;
			}
			break;
		}

		if (startPos == lineStartNext) {
			const FoldLineState foldNext(styler.GetLineState(lineCurrent + 1));
			levelNext = std::max(levelNext, SC_FOLDLEVELBASE);
			if (foldCurrent.lineComment) {
				levelNext += foldNext.lineComment - foldPrev.lineComment;
			} else if (foldCurrent.packageImport) {
				levelNext += foldNext.packageImport - foldPrev.packageImport;
			}

			const int levelUse = levelCurrent;
			int lev = levelUse | (levelNext << 16);
			if (levelUse < levelNext) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			styler.SetLevel(lineCurrent, lev);

			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = std::min(lineStartNext, endPos);
			levelCurrent = levelNext;
			foldPrev = foldCurrent;
			foldCurrent = foldNext;
		}
	}
}

}  // unnamed namespace end

extern const LexerModule lmDart(SCLEX_DART, ColouriseDartDoc, "dart", FoldDartDoc, dartWordListDesc);
