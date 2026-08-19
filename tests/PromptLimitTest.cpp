// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <ai/openai/codex/typed/Conversation.h>

#include <QChar>
#include <QString>

#include <iostream>

namespace {

constexpr qsizetype maximumPromptScalars = static_cast<qsizetype>(
    ai::openai::codex::typed::MaximumTurnInputTextUnicodeScalars);

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= expect(!codexui::FrontendSession::promptValidationError(QStringLiteral("Grüße €")),
                     "a normal UTF-8 prompt must be accepted");

    const QString asciiBoundary(maximumPromptScalars, QLatin1Char('a'));
    passed &= expect(!codexui::FrontendSession::promptValidationError(asciiBoundary),
                     "an ASCII prompt at Codex's exact Unicode-scalar limit must be accepted");
    passed &= expect(codexui::FrontendSession::promptValidationError(asciiBoundary + QLatin1Char('a')).has_value(),
                     "an ASCII prompt one Unicode scalar over Codex's limit must be rejected");

    QString astralBoundary;
    astralBoundary.reserve(maximumPromptScalars * 2);
    for (qsizetype index = 0; index < maximumPromptScalars; ++index)
        astralBoundary.append(QChar::highSurrogate(0x1f642)).append(QChar::lowSurrogate(0x1f642));
    passed &= expect(astralBoundary.size() == maximumPromptScalars * 2,
                     "the astral fixture must use one UTF-16 surrogate pair per Unicode scalar");
    passed &= expect(!codexui::FrontendSession::promptValidationError(astralBoundary),
                     "an astral prompt at Codex's exact Unicode-scalar limit must be accepted");
    passed &= expect(codexui::FrontendSession::promptValidationError(
                         astralBoundary + QString::fromUcs4(U"\U0001f642")).has_value(),
                     "an astral prompt one Unicode scalar over Codex's limit must be rejected");

    const QString mixedBoundary = QString(maximumPromptScalars - 2, QLatin1Char('a'))
                                  + QString::fromUcs4(U"\U0001f642") + QChar(0x20ac);
    passed &= expect(!codexui::FrontendSession::promptValidationError(mixedBoundary),
                     "mixed BMP and astral text must be counted as Unicode scalars");

    return passed ? 0 : 1;
}
