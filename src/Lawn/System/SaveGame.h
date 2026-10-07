/*
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __SAVEGAMECONTEXT_H__
#define __SAVEGAMECONTEXT_H__

#include <span>
#include <string>
#include <vector>

class Board;

bool				LawnLoadGame(Board* theBoard, const std::string& theFilePath);
bool				LawnSaveGame(Board* theBoard, const std::string& theFilePath);
bool				LawnSerializeGameV4(Board* theBoard, std::vector<unsigned char>& theBytes);
// Captures a bounded SAVE4 backup and attempts to restore it if semantic decoding fails.
// The payload still is not safe for untrusted network use until field validation is complete.
bool				LawnLoadGameV4FromMemory(Board* theBoard, std::span<const unsigned char> theBytes);

#endif
