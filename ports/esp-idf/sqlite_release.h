// SPDX-License-Identifier: Apache-2.0
/* IDF can evaluate expressions in disabled assertions. In the SQLite release
 * amalgamation those expressions may reference symbols compiled only in debug. */
#include <assert.h>
#undef assert
#define assert(expr) ((void)0)
