// SPDX-License-Identifier: Apache-2.0
#ifndef MM_MICROPYTHON_ROW_H
#define MM_MICROPYTHON_ROW_H

#include "py/obj.h"
#include "micromeerschaum/document.h"

/* Own the borrowed field names and values before advancing the cursor. */
mp_obj_t mm_python_dict(mm_row row);

#endif
