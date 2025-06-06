/*
 Copyright (C) 2010 Kristian Duske

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

#include "EntityDefinitionParser.h"

#include "Exceptions.h"
#include "Macros.h"
#include "io/EntityDefinitionClassInfo.h"
#include "io/ParserStatus.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityProperties.h"
#include "mdl/ModelDefinition.h"
#include "mdl/PropertyDefinition.h"

#include "kdl/range_to_vector.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace tb::io
{
namespace
{
static constexpr auto DefaultSize = vm::bbox3d(-8, +8);

std::shared_ptr<mdl::PropertyDefinition> mergeAttributes(
  const mdl::PropertyDefinition& inheritingClassAttribute,
  const mdl::PropertyDefinition& superClassAttribute)
{
  assert(inheritingClassAttribute.key() == superClassAttribute.key());

  // for now, only merge spawnflags
  if (
    superClassAttribute.type() == mdl::PropertyDefinitionType::FlagsProperty
    && inheritingClassAttribute.type() == mdl::PropertyDefinitionType::FlagsProperty
    && superClassAttribute.key() == mdl::EntityPropertyKeys::Spawnflags
    && inheritingClassAttribute.key() == mdl::EntityPropertyKeys::Spawnflags)
  {

    const auto& name = inheritingClassAttribute.key();
    auto result = std::make_shared<mdl::FlagsPropertyDefinition>(name);

    const auto& baseclassFlags =
      static_cast<const mdl::FlagsPropertyDefinition&>(superClassAttribute);
    const auto& classFlags =
      static_cast<const mdl::FlagsPropertyDefinition&>(inheritingClassAttribute);

    for (int i = 0; i < 24; ++i)
    {
      const auto* baseclassFlag = baseclassFlags.option(static_cast<int>(1 << i));
      const auto* classFlag = classFlags.option(static_cast<int>(1 << i));

      if (baseclassFlag && !classFlag)
      {
        result->addOption(
          baseclassFlag->value(),
          baseclassFlag->shortDescription(),
          baseclassFlag->longDescription(),
          baseclassFlag->isDefault());
      }
      else if (classFlag)
      {
        result->addOption(
          classFlag->value(),
          classFlag->shortDescription(),
          classFlag->longDescription(),
          classFlag->isDefault());
      }
    }

    return result;
  }

  return nullptr;
}

/**
 * Inherits the attributes from the super class to the inheriting class.
 *
 * Most attributes are only inherited if they are not already present in the inheriting
 * class, except for the following:
 * - spawnflags are merged together
 * - model definitions are merged together
 */
void inheritAttributes(
  EntityDefinitionClassInfo& inheritingClass, const EntityDefinitionClassInfo& superClass)
{
  if (!inheritingClass.description)
  {
    inheritingClass.description = superClass.description;
  }
  if (!inheritingClass.color)
  {
    inheritingClass.color = superClass.color;
  }
  if (!inheritingClass.size)
  {
    inheritingClass.size = superClass.size;
  }

  for (const auto& attribute : superClass.propertyDefinitions)
  {
    auto it = std::ranges::find_if(
      inheritingClass.propertyDefinitions,
      [&](const auto& a) { return a->key() == attribute->key(); });
    if (it == std::end(inheritingClass.propertyDefinitions))
    {
      inheritingClass.propertyDefinitions.push_back(attribute);
    }
    else
    {
      if (auto mergedAttribute = mergeAttributes(**it, *attribute))
      {
        *it = mergedAttribute;
      }
    }
  }

  if (!inheritingClass.modelDefinition)
  {
    inheritingClass.modelDefinition = superClass.modelDefinition;
  }
  else if (superClass.modelDefinition)
  {
    inheritingClass.modelDefinition->append(*superClass.modelDefinition);
  }

  if (!inheritingClass.decalDefinition)
  {
    inheritingClass.decalDefinition = superClass.decalDefinition;
  }
  else if (superClass.decalDefinition)
  {
    inheritingClass.decalDefinition->append(*superClass.decalDefinition);
  }
}

/**
 * Filter out redundant classes. A class is redundant if a class of the same name exists
 * at an earlier position in the given vector, unless the two classes each have one of the
 * types point and brush each. That is, any duplicate is redundant with the exception of
 * overloaded point and brush classes.
 */
std::vector<EntityDefinitionClassInfo> filterRedundantClasses(
  ParserStatus& status, const std::vector<EntityDefinitionClassInfo>& classInfos)
{
  auto result = std::vector<EntityDefinitionClassInfo>{};
  result.reserve(classInfos.size());

  const auto getMask = [](const auto type) { return 1 << static_cast<int>(type); };

  const auto baseClassMask = getMask(EntityDefinitionClassType::BaseClass);

  auto seen = std::unordered_map<std::string, int>{};
  for (const auto& classInfo : classInfos)
  {
    auto& seenMask = seen[classInfo.name];
    const auto classMask = getMask(classInfo.type);

    if (classMask & seenMask)
    {
      status.warn(classInfo.location, "Duplicate class info '" + classInfo.name + "'");
    }
    else if ((seenMask & baseClassMask) || (seenMask != 0 && (classMask & baseClassMask)))
    {
      status.warn(classInfo.location, "Redundant class info '" + classInfo.name + "'");
    }
    else
    {
      result.push_back(classInfo);
      seenMask |= classMask;
    }
  }

  return result;
}

// forward declaration to enable recursion
template <typename F>
void findSuperClassesAndInheritFrom(
  ParserStatus& status,
  EntityDefinitionClassInfo& inheritingClass,
  const EntityDefinitionClassInfo& classWithSuperClasses,
  const F& findClassInfos,
  std::unordered_set<std::string>& visited);

/**
 * Resolves inheritance from the given inheriting class to the given super class, and
 * recurses into the super classes of the given super class.
 *
 * If the given super class has already been visited on the current path from the
 * inheriting class to the super class, then the inheritance hierarchy contains a cycle.
 * In this case, an error is added to the given status object and the recursion stops.
 *
 * Otherwise, the attributes from the given super class are copied to the inheriting
 * class. For the exact semantics of inheriting an attribute from a super class, see the
 * inheritAttributes function. Afterwards, the super classes of the given super class
 * are recursively inherited from.
 *
 * By copying the attributes before recursing further into the super class hierarchy,
 * the attributes inherited from a class that is closer to the inheriting class in the
 * inheritance hierarchy take precedence over the attributes from a class that is
 * further. This means that attributes from the further class get overridden by
 * attributes from the closer class.
 *
 * The following example illustrates this. Let A, B, C be classes such that A inherits
 * from B and B inherits from C. Then B has its attributes copied into A before C. And
 * since attributes are only copied if they are not present (with some exceptions), the
 * attributes from B take precedence over the attributes from C.
 *
 * @param status the parser status to add errors to
 * @param inheritingClass class the class that is currently processed, i.e. the class
 * that induces the inheritance hierarchy that is currently being resolved
 * @param superClass the super class to inherit from
 * @param findClassInfos a function that finds class infos by their names
 * @param visited a set that contains the names of the classes visited so far on the
 * path from the inheriting class to the given super class
 *
 */
template <typename F>
void inheritFromAndRecurse(
  ParserStatus& status,
  EntityDefinitionClassInfo& inheritingClass,
  const EntityDefinitionClassInfo& superClass,
  const F& findClassInfos,
  std::unordered_set<std::string>& visited)
{
  if (visited.insert(superClass.name).second)
  {
    inheritAttributes(inheritingClass, superClass);
    findSuperClassesAndInheritFrom(
      status, inheritingClass, superClass, findClassInfos, visited);

    visited.erase(superClass.name);
  }
  else
  {
    status.error(
      inheritingClass.location, "Entity definition class hierarchy contains a cycle");
  }
}

/**
 * Find the super classes to inherit from, and process each of them by callling
 * `inheritFromAndRecurse`.
 *
 * The given `classWithSuperClasses` is used to determine the super classes to inherit
 * from. This can be the same as the given inheriting class, which is the class that
 * induces the inheritance hierarchy and to which the inherited attributes are added.
 *
 * For each super class name found at `classWithSuperClasses`, the function determines
 * which class should be inherited from. Since there can be multiple classes with the same
 * name, but different types, the following rules are used to resolve ambiguities:
 *
 * - If only one super class with the given name exists, then use that as a super class.
 * - If more than one super class with the given name exists:
 *   - if one of those potential super classes has the same type as the given inheriting
 * class, then use it as a super class.
 *   - if the given inheriting class is not of type BaseClass, and one of the potential
 * super classes is of type BaseClass, then use it as a super class. Otherwise, no super
 * class was found, return null.
 *
 * If a super class was found, inherit its attributes and recurse into its super classes
 * again by calling `inheritFromAndRecurse`.
 *
 * If the given `classWithSuperClasses` has multiple super classes, they are processed in
 * the order in which they were declared. This gives precedence to the attributes
 * inherited from a super class that was declared at a lower position than another super
 * class.
 *
 * @param status the parser status to add errors to
 * @param inheritingClass class the class that is currently processed, i.e. the class that
 * induces the inheritance hierarchy that is currently being resolved
 * @param classWithSuperClasses the class that declares the super classes to inherit from
 * @param findClassInfos a function that finds class infos by their names
 * @param visited a set that contains the names of the classes visited so far on the path
 * from the inheriting class to the given super class
 */
template <typename F>
void findSuperClassesAndInheritFrom(
  ParserStatus& status,
  EntityDefinitionClassInfo& inheritingClass,
  const EntityDefinitionClassInfo& classWithSuperClasses,
  const F& findClassInfos,
  std::unordered_set<std::string>& visited)
{
  const auto findClassInfoWithType =
    [&](const auto& classes, const auto type) -> const EntityDefinitionClassInfo* {
    if (const auto it = std::find_if(
          classes.begin(), classes.end(), [&](const auto* c) { return c->type == type; });
        it != classes.end())
    {
      return *it;
    }
    return nullptr;
  };

  const auto selectSuperClass =
    [&](const auto& potentialSuperClasses) -> const EntityDefinitionClassInfo* {
    if (potentialSuperClasses.size() == 1u)
    {
      return potentialSuperClasses.front();
    }
    if (potentialSuperClasses.size() > 1u)
    {
      // find a super class with the same class type as the inheriting class
      if (
        const auto* classInfo =
          findClassInfoWithType(potentialSuperClasses, inheritingClass.type))
      {
        return classInfo;
      }

      if (inheritingClass.type != EntityDefinitionClassType::BaseClass)
      {
        // find a super class of type BaseClass
        if (
          const auto* classInfo = findClassInfoWithType(
            potentialSuperClasses, EntityDefinitionClassType::BaseClass))
        {
          return classInfo;
        }
      }
    }

    return nullptr;
  };

  for (const auto& nextSuperClassName : classWithSuperClasses.superClasses)
  {
    if (const auto* nextSuperClass = selectSuperClass(findClassInfos(nextSuperClassName)))
    {
      inheritFromAndRecurse(
        status, inheritingClass, *nextSuperClass, findClassInfos, visited);
    }
    else
    {
      status.error(
        classWithSuperClasses.location,
        "No matching super class found for '" + nextSuperClassName + "'");
    }
  }
}

/**
 * Resolves the inheritance hierarchy induced the given inheriting class by recursively
 * inheriting attributes from its super classes.
 *
 * The super classes are explored in a depth first order, with super classes of a given
 * class being explored in the order in which they were declared. Once an attribute has
 * been inherited from some super class, it takes precedence over an attribute of the same
 * name in some other super class that is visited later in the process.
 *
 * @param status the parser status to add errors to
 * @param inheritingClass class the class that is currently processed, i.e. the class that
 * induces the inheritance hierarchy that is currently being resolved
 * @param findClassInfos a function that finds class infos by their names
 * @return a copy of the given inheriting class, with all attributes it inherits from its
 * super classes added
 */
template <typename F>
EntityDefinitionClassInfo resolveInheritance(
  ParserStatus& status,
  EntityDefinitionClassInfo inheritingClass,
  const F& findClassInfos)
{
  auto visited = std::unordered_set<std::string>();
  findSuperClassesAndInheritFrom(
    status, inheritingClass, inheritingClass, findClassInfos, visited);
  return inheritingClass;
}

std::unique_ptr<mdl::EntityDefinition> createDefinition(
  EntityDefinitionClassInfo classInfo, const Color& defaultEntityColor)
{
  auto name = std::move(classInfo.name);
  auto color = std::move(classInfo.color).value_or(defaultEntityColor);
  auto size = std::move(classInfo.size).value_or(DefaultSize);
  auto description = std::move(classInfo.description).value_or("");
  auto propertyDefinitions = std::move(classInfo.propertyDefinitions);
  auto modelDefinition =
    std::move(classInfo.modelDefinition).value_or(mdl::ModelDefinition{});
  auto decalDefinition =
    std::move(classInfo.decalDefinition).value_or(mdl::DecalDefinition{});

  switch (classInfo.type)
  {
  case EntityDefinitionClassType::PointClass:
    return std::make_unique<mdl::PointEntityDefinition>(
      std::move(name),
      color,
      size,
      std::move(description),
      std::move(propertyDefinitions),
      std::move(modelDefinition),
      std::move(decalDefinition));
  case EntityDefinitionClassType::BrushClass:
    return std::make_unique<mdl::BrushEntityDefinition>(
      std::move(name), color, std::move(description), std::move(propertyDefinitions));
  case EntityDefinitionClassType::BaseClass:
    return nullptr;
    switchDefault();
  };
}

std::vector<std::unique_ptr<mdl::EntityDefinition>> createDefinitions(
  ParserStatus& status,
  const std::vector<EntityDefinitionClassInfo>& classInfos,
  const Color& defaultEntityColor)
{
  const auto resolvedClasses =
    resolveInheritance(status, filterRedundantClasses(status, classInfos));

  auto result = std::vector<std::unique_ptr<mdl::EntityDefinition>>{};
  for (auto classInfo : resolvedClasses)
  {
    if (auto definition = createDefinition(std::move(classInfo), defaultEntityColor))
    {
      result.push_back(std::move(definition));
    }
  }

  return result;
}

} // namespace

/**
 * Resolves the inheritance for every class that is not of type BaseClass in the given
 * vector and returns a vector of copies where the inherited attributes are added to the
 * inheriting classes.
 *
 * Exposed for testing.
 */
std::vector<EntityDefinitionClassInfo> resolveInheritance(
  ParserStatus& status, const std::vector<EntityDefinitionClassInfo>& classInfos)
{
  const auto filteredClassInfos = filterRedundantClasses(status, classInfos);
  const auto findClassInfos =
    [&](const auto& name) -> std::vector<const EntityDefinitionClassInfo*> {
    return filteredClassInfos
           | std::views::filter([&](const auto& c) { return c.name == name; })
           | std::views::transform([](const auto& c) { return &c; }) | kdl::to_vector;
  };

  return filteredClassInfos | std::views::filter([](const auto& c) {
           return c.type != EntityDefinitionClassType::BaseClass;
         })
         | std::views::transform(
           [&](const auto& c) { return resolveInheritance(status, c, findClassInfos); })
         | kdl::to_vector;
}

EntityDefinitionParser::EntityDefinitionParser(const Color& defaultEntityColor)
  : m_defaultEntityColor{defaultEntityColor}
{
}

EntityDefinitionParser::~EntityDefinitionParser() {}

Result<std::vector<std::unique_ptr<mdl::EntityDefinition>>> EntityDefinitionParser::
  parseDefinitions(ParserStatus& status)
{
  try
  {
    const auto classInfos = parseClassInfos(status);
    return createDefinitions(status, classInfos, m_defaultEntityColor);
  }
  catch (const Exception& e)
  {
    return Error{e.what()};
  }
}

} // namespace tb::io
