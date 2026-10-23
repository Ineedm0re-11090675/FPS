# Generic Reflection, World, and Structured Data Tooling for VibeUE

## Motivation

Recent Unreal automation work exposed a recurring pattern in VibeUE:

- high-level MCP access is still dominated by `execute_python_code`
- many editor operations ultimately depend on `ImportText_Direct`
- service boundaries are asset-specific instead of type-system-centric

That is workable for simple scalar edits, but it breaks down for richer editor automation:

- setting `WorldSettings.default_game_mode` to a Blueprint-generated class
- creating and mutating `DataTable` assets backed by Blueprint row structs
- placing or updating actors directly in a map asset in headless/editor-automation contexts
- setting nested `struct/enum/object/class/soft object/soft class/array/map` properties without relying on Unreal text import syntax

The sharpness demo workflow hit all three problems in one chain:

1. a duplicated demo map needed a project-specific `GameMode`
2. the `GameMode` had to be a Blueprint-generated class
3. the demo data wanted a Blueprint-authored `DataTable` row struct
4. map-scoped actor injection needed to work without editor viewport assumptions

These are not sharpness-specific needs. They are generic automation gaps.

## Current State

The current plugin already has useful service coverage:

- [PythonToolsRegistration.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/PythonToolsRegistration.cpp)
- [UActorService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UActorService.cpp)
- [UDataAssetService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UDataAssetService.cpp)
- [UDataTableService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UDataTableService.cpp)
- [UEngineSettingsService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UEngineSettingsService.cpp)
- [UBlueprintService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UBlueprintService.cpp)

The gap is architectural, not purely API count:

- `UDataAssetService` and `UEngineSettingsService` mainly convert through `ImportText_Direct`
- `UActorService` is centered on the current editor world, not asset-level level editing
- `UDataTableService::FindRowStruct(...)` filters to `FTableRowBase` descendants, which excludes practical Blueprint-row cases
- top-level tool exposure is still Python-heavy instead of surfacing typed native operations directly

## Design Goals

The next tool layer should be:

1. generic rather than sharpness-specific
2. typed rather than string-import-driven
3. usable from both Python services and direct MCP tools
4. safe for headless/editor-automation workflows
5. able to target both live objects and asset-backed objects
6. composable, so domain-specific services become thin wrappers

## Proposed Architecture

The missing foundation is a shared typed reflection kernel. Most specialized services should delegate into it instead of each reinventing text import/export.

### Layer 1: Typed Reflection Kernel

Add a generic reflection core that can inspect, read, and write property values without requiring callers to speak Unreal text syntax.

Suggested files:

- `Public/Reflection/VibeTypedValue.h`
- `Private/Reflection/VibeTypedValue.cpp`
- `Public/Reflection/VibePropertyPath.h`
- `Private/Reflection/VibePropertyPath.cpp`
- `Public/Reflection/VibeReflectionUtils.h`
- `Private/Reflection/VibeReflectionUtils.cpp`

Core responsibilities:

- resolve object by object path, asset path, class path, or live world lookup
- resolve nested property paths such as:
  - `default_game_mode`
  - `hud_class`
  - `MyStruct.SomeField`
  - `ModulesDefinitionList[0].ModuleWidgetClass`
  - `MapProperty["Demo"].TargetContext`
- export property schema
- read property value as typed JSON
- write property value from typed JSON
- support batch mutation with per-field success/failure reporting

### Layer 2: World/Level Asset Editing

Split "current editor world actor manipulation" from "edit a map asset safely".

Suggested files:

- `Public/PythonAPI/UWorldAssetService.h`
- `Private/PythonAPI/UWorldAssetService.cpp`

This layer should operate on:

- map assets
- duplicated worlds
- world settings
- level actors/components inside a map package

It should not depend on viewport spawning.

### Layer 3: Structured Data Editing

Unify DataTables, DataAssets, and similar content objects around the reflection kernel instead of per-asset string parsing.

Suggested files:

- `Public/PythonAPI/UStructuredDataService.h`
- `Private/PythonAPI/UStructuredDataService.cpp`

This service would cover:

- `DataAsset`
- `PrimaryDataAsset`
- `DataTable`
- Blueprint-authored row structs
- batch row upsert/query
- schema inspection

### Layer 4: Domain-Specific Thin Services

After the three base layers above exist, specialized services become much thinner:

- EasyHudBuilder service
- project/global config service
- widget-specific service
- settings/category wrappers

These should become convenience APIs, not foundational plumbing.

## Typed Value Model

The key change is to stop pushing raw strings as the write contract.

Suggested JSON envelope:

```json
{
  "kind": "class",
  "class_path": "/Game/ProjectUI/.../BP_HPI_WeaponSharpnessDemoGameMode.BP_HPI_WeaponSharpnessDemoGameMode_C"
}
```

Supported kinds:

- `null`
- `bool`
- `int`
- `float`
- `string`
- `name`
- `text`
- `enum`
- `object`
- `soft_object`
- `class`
- `soft_class`
- `struct`
- `array`
- `map`
- `set`

Examples:

```json
{ "kind": "enum", "enum_path": "/Game/.../E_HPI_SharpnessEvent.E_HPI_SharpnessEvent", "value": "Damage" }
```

```json
{ "kind": "object", "object_path": "/Game/ProjectUI/.../DA_HPI_WeaponSharpnessProfileRegistry.DA_HPI_WeaponSharpnessProfileRegistry" }
```

```json
{
  "kind": "struct",
  "struct_path": "/Game/.../F_HPI_SharpnessProfileRegistryEntry.F_HPI_SharpnessProfileRegistryEntry",
  "fields": {
    "WeaponId": { "kind": "name", "value": "Bow_01" },
    "Label": { "kind": "string", "value": "Bow" },
    "ProfileAssetPath": {
      "kind": "soft_object",
      "object_path": "/Game/.../DA_HPI_WeaponSharpnessProfile_Bow_01.DA_HPI_WeaponSharpnessProfile_Bow_01"
    }
  }
}
```

```json
{
  "kind": "array",
  "items": [
    { "kind": "string", "value": "A" },
    { "kind": "string", "value": "B" }
  ]
}
```

This removes the need for callers to guess Unreal import text formatting and makes class/object/struct writes deterministic.

## Proposed Generic MCP Tools

These tools should be added as first-class native tools, not hidden behind Python execution.

### Reflection Tools

#### `get_object_schema`

Input:

- `target`
- `include_inherited`
- `max_depth`

Output:

- property list
- type info
- writable flag
- path syntax hints

Use cases:

- inspect `WorldSettings`
- inspect config objects
- inspect `PrimaryDataAsset` schema
- inspect Blueprint CDO schema

#### `get_object_properties`

Input:

- `target`
- `paths`

Output:

- typed values by path
- missing-path diagnostics

#### `set_object_properties`

Input:

- `target`
- `values`
- `save`

Output:

- per-path success/failure
- property-level diagnostics

This is the generic answer to current `SetProperty`-style operations.

### World/Map Tools

#### `get_map_world_settings`

Input:

- `map_path`

Output:

- world settings object path
- typed property schema/value snapshot

#### `set_map_world_settings`

Input:

- `map_path`
- `values`
- `save`

This must support Blueprint-generated classes in `ClassProperty` and `SoftClassProperty`.

#### `list_map_actors`

Input:

- `map_path`
- `class_filter`
- `label_filter`

Output:

- actors in the level package, not just the currently open editor world

#### `spawn_actor_in_map`

Input:

- `map_path`
- `actor_class`
- `transform`
- `label`
- `folder_path`
- `select`

Output:

- actor object path / GUID / label

This solves headless-safe map mutation without `EditorActorSubsystem.spawn_actor_from_class`.

#### `update_actor_in_map`

Input:

- `map_path`
- `actor_identifier`
- `values`
- `transform`

#### `remove_actor_from_map`

Input:

- `map_path`
- `actor_identifier`

### Structured Data Tools

#### `create_structured_asset`

Input:

- `asset_class`
- `asset_path`
- `defaults`

This should support Blueprint-generated `DataAsset` classes and other UObject asset classes via typed defaults.

#### `create_data_table`

Input:

- `asset_path`
- `row_struct`

The `row_struct` contract must accept:

- native row structs
- `UUserDefinedStruct`
- object paths, not only names

#### `get_data_table_schema`

Input:

- `table_path`

Output:

- row struct metadata
- column schema
- row count

#### `mutate_data_table_rows`

Input:

- `table_path`
- `operations`
- optional `rows_json` compatibility shorthand for pure upsert

Output:

- per-row success/failure for `upsert`, `delete`, and `rename`

#### `get_data_table_rows`

Input:

- `table_path`
- `row_names`

Output:

- typed row payloads

## Why These Tools Generalize Well

These APIs are not special to HUD work. They also cover:

- setting `GameMode`, `HUDClass`, `DefaultPawnClass`, `PlayerControllerClass`
- writing project/global config assets
- mutating EasyHudBuilder definitions
- filling DataTables backed by Blueprint structs
- batch authoring DataAssets
- editing live config objects and Blueprint CDOs
- automated map setup for demos, tests, and templates

In other words, the goal is not "add a sharpness tool". The goal is "make VibeUE understand Unreal typed data and map assets as first-class automation targets".

## Concrete Changes to Existing Services

### `UDataTableService`

Current issue:

- [UDataTableService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UDataTableService.cpp) hard-filters row structs to `FTableRowBase`

Proposed change:

- replace `FindRowStruct(...)` with a broader compatibility check
- accept:
  - native `UScriptStruct` row structs
  - editor-valid `UUserDefinedStruct`
- move row read/write through typed reflection instead of plain JSON-string-to-row import only

### `UActorService`

Current issue:

- [UActorService.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/PythonAPI/UActorService.cpp) assumes the active editor world

Proposed change:

- keep `UActorService` for live editor-world workflows
- add `UWorldAssetService` for asset-level map mutation

### `UDataAssetService` and `UEngineSettingsService`

Current issue:

- both rely heavily on `ImportText_Direct`

Proposed change:

- keep string convenience overloads for backward compatibility
- route them internally through the typed reflection kernel
- add richer overloads that accept typed JSON values directly

### `PythonToolsRegistration`

Current issue:

- high-value operations remain trapped behind `execute_python_code`

Proposed change:

- add first-class MCP registrations for the new reflection/world/structured-data tools
- keep Python as escape hatch, not primary path

## Suggested Rollout

### Phase 1: Reflection Kernel

Ship:

- typed value schema
- object/property schema inspection
- typed property get/set
- batch property mutation

This unlocks:

- class/object/enum/struct writes
- safer config and asset editing

### Phase 2: World Asset Editing

Ship:

- map world settings read/write
- list/spawn/update/remove actors in a map asset

This unlocks:

- demo-map bootstrap
- safe automated room/test scene generation

### Phase 3: Structured Data

Ship:

- DataTable row-struct compatibility fixes
- typed row upsert/get
- unified structured asset creation

This unlocks:

- Blueprint-authored table pipelines
- profile registries and content-authoring flows

### Phase 4: Thin Domain Adapters

Ship:

- EasyHudBuilder-specific helpers
- project-global-config helpers
- higher-level wrappers for common authoring tasks

## Validation Matrix

The following cases should be used as acceptance tests for the new tooling:

1. set duplicated map `default_game_mode` to a Blueprint-generated class
2. set a Blueprint `HUDClass` on a Blueprint `GameMode` CDO
3. create a `DataTable` from a Blueprint-authored row struct and read rows back without `FallbackStruct`
4. spawn a Blueprint actor into a duplicated map without requiring the editor viewport
5. batch-edit a `PrimaryDataAsset` containing arrays, maps, enums, structs, soft object refs, and class refs
6. update EasyHudBuilder module definitions via typed property paths rather than raw text import

## Non-Goals

This proposal does not require removing Python tooling. Python should remain:

- a discovery surface
- a debugging escape hatch
- a fast way to prototype service behavior

But it should no longer be the only route to advanced editor automation.

## Recommendation

VibeUE should prioritize a generic typed reflection foundation before adding more narrow one-off tools.

The sharpness-demo blockers were useful because they exposed three systemic gaps at once:

- typed property setting for Blueprint classes
- asset-level world editing
- Blueprint-row structured data fidelity

Fixing those generically will unlock a much wider set of authoring workflows than any single project-specific patch.
