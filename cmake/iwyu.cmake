# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

find_program(iwyu_path NAMES include-what-you-use iwyu)

if(iwyu_path)
    option(CHECK_INCLUDES "Check used headers" OFF)
    if(CHECK_INCLUDES)
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
            ${iwyu_path}
            -std=c++20
            -Xiwyu
            --verbose=3
            -Xiwyu
            --cxx17ns
            -Xiwyu
            --quoted_includes_first
            -Xiwyu
            --max_line_length=160
        )
    endif()
else()
    message(
        WARNING
            "Include-what-you-use (iwyu) is needed for checking include dependencies"
    )
endif()
