/*
 Copyright (C) 2010-2017 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "Result.h"

#include <filesystem>
#include <vector>

namespace TrenchBroom::Assets
{
class EntityDefinition;
} // namespace TrenchBroom::Assets

namespace TrenchBroom::IO
{
class ParserStatus;

class EntityDefinitionLoader
{
public:
  virtual ~EntityDefinitionLoader();
  Result<std::vector<Assets::EntityDefinition*>> loadEntityDefinitions(
    ParserStatus& status, const std::filesystem::path& path) const;

private:
  virtual Result<std::vector<Assets::EntityDefinition*>> doLoadEntityDefinitions(
    ParserStatus& status, const std::filesystem::path& path) const = 0;
};
} // namespace TrenchBroom::IO
