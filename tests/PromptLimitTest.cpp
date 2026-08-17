// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "app/FrontendSession.h"

#include <QChar>
#include <QString>

#include <iostream>

namespace {

constexpr qsizetype maximumPromptBytes = 128 * 1024;

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

    const QString asciiBoundary(maximumPromptBytes, QLatin1Char('a'));
    passed &= expect(asciiBoundary.toUtf8().size() == maximumPromptBytes,
                     "the ASCII boundary fixture must encode to exactly 128 KiB");
    passed &= expect(!codexui::FrontendSession::promptValidationError(asciiBoundary),
                     "an exactly 128 KiB ASCII prompt must be accepted");
    passed &= expect(codexui::FrontendSession::promptValidationError(asciiBoundary + QLatin1Char('a')).has_value(),
                     "an ASCII prompt one byte over the limit must be rejected");

    const QString multibyteBoundary = QString(43'690, QChar(0x20ac)) + QStringLiteral("ab");
    passed &= expect(multibyteBoundary.toUtf8().size() == maximumPromptBytes,
                     "the multibyte boundary fixture must encode to exactly 128 KiB");
    passed &= expect(!codexui::FrontendSession::promptValidationError(multibyteBoundary),
                     "an exactly 128 KiB multibyte prompt must be accepted");
    passed &= expect(codexui::FrontendSession::promptValidationError(multibyteBoundary + QLatin1Char('x')).has_value(),
                     "a multibyte prompt one byte over the limit must be rejected");

    return passed ? 0 : 1;
}
