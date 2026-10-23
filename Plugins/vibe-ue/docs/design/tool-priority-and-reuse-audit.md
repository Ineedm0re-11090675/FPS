# VibeUE Tool Priority and Reuse Audit

## Purpose

This document ranks VibeUE tooling by practical reuse, not by implementation count.

The goal is to answer:

- which tool families are actually high-frequency in day-to-day Unreal authoring
- which existing services are underexposed as MCP tools
- which investments will unlock the broadest range of workflows

This audit is intentionally generic. It is not scoped to the sharpness HUD work.

## Evidence Sources

### Top-Level MCP Tools Currently Registered

Current first-class MCP registrations are concentrated in a small subset of the plugin:

- [PythonToolsRegistration.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/PythonToolsRegistration.cpp)
- [WidgetBlueprintTool.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WidgetBlueprintTool.cpp)
- [WidgetDesignerTool.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WidgetDesignerTool.cpp)
- [WidgetLogicTools.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WidgetLogicTools.cpp)
- [WidgetSpecTools.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WidgetSpecTools.cpp)
- [ContextTools.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/ContextTools.cpp)
- [LogReaderToolsRegistration.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/LogReaderToolsRegistration.cpp)
- [TextureImportTool.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/TextureImportTool.cpp)
- [NiagaraSystemTool.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/NiagaraSystemTool.cpp)
- [SkillsTools.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/SkillsTools.cpp)

The registered set is currently dominated by:

- Python discovery and execution
- widget-specific workflows
- context/logs
- one-off domain tools like texture import and niagara systems

### Existing Service Surface

There are significantly more reusable service families already present under `Public/PythonAPI`:

- [UAssetDiscoveryService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UAssetDiscoveryService.h)
- [UBlueprintService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UBlueprintService.h)
- [UDataAssetService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UDataAssetService.h)
- [UDataTableService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UDataTableService.h)
- [UEngineSettingsService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UEngineSettingsService.h)
- [UProjectSettingsService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UProjectSettingsService.h)
- [UActorService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UActorService.h)
- [UScreenshotService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UScreenshotService.h)
- [UMaterialService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UMaterialService.h)
- [UMaterialNodeService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UMaterialNodeService.h)
- [ULandscapeService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/ULandscapeService.h)
- [ULandscapeMaterialService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/ULandscapeMaterialService.h)
- [UInputService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UInputService.h)
- [UEnumStructService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UEnumStructService.h)
- [UNiagaraService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UNiagaraService.h)
- [UNiagaraEmitterService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UNiagaraEmitterService.h)
- [UAnimSequenceService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UAnimSequenceService.h)
- [UAnimMontageService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UAnimMontageService.h)
- [UAnimGraphService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UAnimGraphService.h)
- [USkeletonService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/USkeletonService.h)
- [UFoliageService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UFoliageService.h)
- [URuntimeVirtualTextureService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/URuntimeVirtualTextureService.h)
- [UWidgetService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UWidgetService.h)
- [UWidgetDesignerService.h](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Public/PythonAPI/UWidgetDesignerService.h)

This means the plugin already has a broad capability base, but much of it is not exposed as convenient, typed, first-line tools.

### Skill and Test Prompt Coverage

The plugin content strongly hints which domains are expected to be used often:

- skills under [Content/Skills](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Content/Skills)
- prompt suites under [test_prompts](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/test_prompts)

Notable recurring domains in both:

- widgets / UMG
- blueprints
- assets
- data assets
- data tables
- level actors
- project settings / engine settings
- landscape / landscape materials
- materials
- enhanced input
- niagara
- animation

This overlap is a good proxy for intended high-frequency authoring coverage.

## Key Finding

There is a mismatch between:

- what the plugin claims to support through services and skills
- what is actually easy to use through top-level MCP tools

In particular, several high-frequency, high-reuse domains still fall back to Python-heavy orchestration:

- generic asset discovery and mutation
- generic Blueprint introspection/editing
- structured data authoring
- map/world editing
- settings/property editing
- screenshots/captures

## Priority Framework

Tool families should be ranked by two dimensions:

1. reuse across domains
2. likelihood of being on the critical path of an authoring task

### Tier P0: Foundational, Cross-Domain, High-Frequency

These are the most important. They unlock many workflows and reduce Python dependence.

#### 1. Typed Reflection and Property Editing

Status:

- partially present through asset/settings services
- currently too string-driven

Why P0:

- every content type eventually becomes property read/write
- unblocks Blueprint class refs, struct fields, arrays, maps, config objects, CDOs, data assets, builder definitions

Recommendation:

- implement the typed reflection kernel proposed in [generic-reflection-world-data-tooling.md](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/docs/design/generic-reflection-world-data-tooling.md)
- promote `get_object_schema`, `get_object_properties`, `set_object_properties` to first-class MCP tools

#### 2. Asset Discovery and Object Resolution

Status:

- strong service exists via `UAssetDiscoveryService`
- not elevated enough as a primary MCP workflow surface

Why P0:

- nearly every task starts by locating assets, classes, dependencies, or folders
- this is the backbone for widgets, blueprints, materials, maps, niagara, animations

Recommendation:

- expose first-class tools for:
  - search assets
  - inspect asset
  - get dependencies/references
  - open asset/editor

#### 3. Blueprint Introspection and Editing

Status:

- `UBlueprintService` is broad
- workflow still often falls back to Python or domain-specific wrappers

Why P0:

- Blueprint work is the real center of authoring in many UE projects
- applies to actors, game modes, UI wrappers, input handlers, demo controllers, inventory/presenter assets

Recommendation:

- promote generic Blueprint tools beyond widget-only flows:
  - create blueprint
  - inspect graphs/functions/variables/components
  - add variables/functions/events
  - set defaults on Blueprint CDO

#### 4. World/Map Editing

Status:

- `UActorService` exists, but it is editor-world-centric

Why P0:

- demo maps, test scenes, automation harnesses, content staging, and room-based showcases all depend on it
- current viewport/editor-world assumption is too narrow

Recommendation:

- add asset-level world/map tools:
  - inspect world settings
  - set world settings
  - list actors in a map asset
  - spawn/update/remove actors in a map asset

#### 5. Structured Data Authoring

Status:

- `UDataAssetService` and `UDataTableService` exist
- current DataTable path is not robust for Blueprint row structs

Why P0:

- data-driven authoring is common across HUD profiles, registries, gameplay tables, content catalogs, config banks
- reuse is very high even when the domain changes

Recommendation:

- unify DataAsset/DataTable editing behind structured typed-value APIs
- support Blueprint-authored row structs as first-class citizens

#### 6. Context, Logs, and Screenshot Capture

Status:

- context and log tools are already first-class
- screenshot functionality exists as service but is underexposed compared with its utility

Why P0:

- every debugging and review loop depends on contextual inspection
- screenshots are critical for UI, materials, environments, and animation reviews

Recommendation:

- keep context/logs as-is
- elevate screenshot and capture flows to first-class MCP tools if not already surfaced equivalently

## Tier P1: High-Value Domain Tooling

These are still important and frequently useful, but they are more domain-bound than P0.

### 1. Widget and UMG Tooling

Status:

- strongest current MCP area

Why P1 and not P0:

- extremely important in UI-heavy projects
- but narrower than reflection/asset/blueprint foundations

Recommendation:

- keep investing
- ensure widget tools consume the new reflection/object-resolution layer instead of duplicating low-level logic

### 2. Materials and Landscape Materials

Status:

- service-rich
- top-level tooling and workflow guidance are more fragmented

Why P1:

- high reuse in environment and lookdev work
- less universal than assets/blueprints/data

Recommendation:

- expose a more opinionated top-level material workflow:
  - inspect material graph
  - create material
  - add common node patterns
  - create auto landscape material

### 3. Landscape

Status:

- broad service surface
- likely high value in worldbuilding tasks

Why P1:

- heavy but domain-specific

Recommendation:

- continue service investment
- keep semantic tools, not only raw vertex tools

### 4. Enhanced Input, Enum/Struct, Project Settings

Status:

- services exist
- highly reusable support domains

Why P1:

- these are enabling systems used across many features
- they benefit heavily from typed reflection and Blueprint tools

Recommendation:

- reframe them as thin workflow tools built on top of P0 foundations

## Tier P2: Specialized but Valuable

These should remain supported, but they are not where general-purpose tooling investment should start.

- Niagara systems and emitters
- animation blueprint / montage / sequence
- skeleton tooling
- foliage
- runtime virtual texture

These are important domains, but they are narrower and often sequence behind P0/P1 tooling in real authoring flows.

## Mismatch Inventory

The following gaps are especially important because they affect high-frequency workflows while lower-reuse tools already have direct MCP exposure.

### Gap 1: Python Is Still the Generic Escape Hatch

Evidence:

- top-level registration includes five Python-centric tools in [PythonToolsRegistration.cpp](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/PythonToolsRegistration.cpp)

Problem:

- generic editing flows still depend on Python orchestration instead of typed native operations

### Gap 2: Many Foundational Services Are Not First-Class MCP Tools

Examples:

- `UAssetDiscoveryService`
- `UBlueprintService`
- `UDataAssetService`
- `UDataTableService`
- `UEngineSettingsService`
- `UProjectSettingsService`
- `UScreenshotService`

Problem:

- capability exists, but ergonomics and reliability are weaker than they should be

### Gap 3: Widget Tooling Is Richer Than the Shared Foundations It Depends On

Problem:

- widget workflows are strong, but generic object/property/data/world editing is weaker
- this leads to brittle project-specific scripting around otherwise strong widget authoring

## Context and Python Surface

These two families should not be treated the same way.

### ContextTools

Current top-level context tools:

- `context_overview`
- `editor_context`
- `project_info`
- `current_level`
- `selected_assets`
- `open_editors`
- `self_test`

Assessment:

- this family is already reasonably shaped
- `editor_context` is the umbrella snapshot
- the others are narrow read slices

Recommendation:

- add a lightweight super-entry such as `context_overview`
- keep them separate for now
- do not collapse them into one giant `manage_context` tool

Reasoning:

- `context_overview` supports the common "rough scan first" workflow
- they are read-only
- they have different payload sizes
- they are often used for token-bounded grounding
- they represent different caller intent:
  - `project_info` is stable metadata
  - `current_level` is level-focused
  - `selected_assets` is content-browser focused
  - `open_editors` is tab/editor focused
  - `self_test` is operational health

This is a good example of where more than one tool is justified.

### PythonToolsRegistration

Current top-level python tools should be shaped as:

- `execute_python_code`
- `discover_python_api`

Assessment:

- this family is too fragmented for a low-level escape hatch surface
- all four discovery tools are variants of the same discovery problem
- the split increases tool count without creating meaningful resource boundaries

Recommendation:

- keep `execute_python_code` as a separate escape hatch
- keep discovery unified under `discover_python_api`

Suggested shape:

- `scope = module | class | function | subsystems`
- `target`
- optional filters
- optional limits

This would preserve capability while reducing tool-surface sprawl.

### General Principle

Context slice tools are justified when:

- they are read-only
- they are cheap
- they reduce token cost
- they represent clearly different grounding intents

Escape-hatch/discovery tools should be consolidated when:

- they operate on the same low-level substrate
- they differ mostly by inspection mode
- they are not distinct resources from a caller perspective

## Recommended Investment Order

### Step 1

Build and expose:

- typed reflection kernel
- generic object schema/get/set tools
- object/class/soft-object/class-path resolution

### Step 2

Build and expose:

- world/map asset editing tools
- world settings class/property tools
- asset-safe actor spawn/update/remove tools

### Step 3

Build and expose:

- structured data tools
- DataTable row schema/query/upsert
- Blueprint row struct support

### Step 4

Promote existing service families to top-level MCP tools:

- asset discovery
- Blueprint editing
- screenshots
- project/engine settings

### Step 5

Refactor domain-specific tools to sit on those foundations:

- widgets
- EasyHudBuilder workflows
- landscape/material helpers
- input/config helpers

## Practical Rule for Future Tool Work

Before adding a new project-specific or domain-specific MCP tool, ask:

1. can this be solved by a stronger generic reflection/object/data/world primitive
2. will at least three unrelated workflows reuse this capability
3. does this remove a current dependency on Python string import or viewport-dependent editor behavior

If the answer is yes, it belongs in the shared foundation first.

## Recommendation

The highest-leverage direction for VibeUE is:

- fewer narrow tools
- more typed generic primitives
- more first-class MCP exposure for already-existing high-value services

That will improve both reuse and actual problem-solving power more than continuing to add isolated wrappers around specialized domains.
