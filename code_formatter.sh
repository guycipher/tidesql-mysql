#!/bin/bash
#
# Before submitting a PR, run this script to format the source code.
#
# It applies the .clang-format in the repository root.  Do not run clang-format
# without that file present -- its own defaults are a different style, and it
# will rewrite every source file in the tree.

set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f .clang-format ]; then
    echo "error: .clang-format is missing from the repository root." >&2
    echo "       Formatting without it would rewrite the whole tree." >&2
    exit 1
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format is not installed." >&2
    exit 1
fi

# -type f applies to the whole group, and the exclusions are one extended-regex
# alternation.  Written as a single -name test each, the group is what keeps
# -type f from binding only to the first pattern.
find . -type f \( -name "*.cc" -o -name "*.h" \) \
    | grep -Ev '(^|/)(external|build|build-[^/]*|cmake-build-[^/]*|\.idea|\.vs|\.vscode)/' \
    | xargs clang-format -i

echo "formatted $(find . -type f \( -name '*.cc' -o -name '*.h' \) \
    | grep -Evc '(^|/)(external|build|build-[^/]*|cmake-build-[^/]*|\.idea|\.vs|\.vscode)/') files"
