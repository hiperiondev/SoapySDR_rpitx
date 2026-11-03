/**
 * @file util.hpp
 * @brief Diagnostic output of librpitx.
 *
 * @copyright Copyright (C) 2018 Evariste COURJAUD F5OEO
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DEF_UTIL
#define DEF_UTIL

#include <stdio.h>
#include <stdarg.h>

/**
 * @brief Sets the diagnostic level of the library (process wide).
 * @param Level 0 prints errors only, 1 adds configuration details, 2 adds register dumps.
 */
void dbg_setlevel(int Level);

/**
 * @brief Returns the diagnostic level.
 * @return Level set by dbg_setlevel() (0 by default).
 */
int dbg_getlevel();

/**
 * @brief Prints a message on stderr when @p Level is less than or equal to the diagnostic level.
 * @param Level Level of the message.
 * @param fmt   printf() format.
 * @param ...   Format arguments.
 */
void dbg_printf(int Level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
