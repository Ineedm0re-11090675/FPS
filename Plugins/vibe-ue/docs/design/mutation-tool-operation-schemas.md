# Mutation Tool Operation Schemas

## Purpose

This document serves two audiences at the same time:

1. caller-facing contract
2. agent-facing decision guidance

That split is intentional.

For humans, this document answers:

- what payload shape does the tool expect
- what operations are supported
- what does a successful response look like

For AI agents, this document answers:

- when to choose a mutation tool instead of several narrow tools
- how to decide between a mutation tool and a read-only tool
- what operation granularity is considered "correct" for this tool family

It is not only for end users, and it is not only an internal style guide. It is both.

## Selection Rule

Use a mutation tool when all of the following are true:

- the resource is the same
- the save semantics are the same
- the restore/transaction semantics are the same
- the only thing changing is the mutation verb

Do not merge read-only inspection and creation/mutation into the same tool unless there is a very strong reason.

Examples:

- `mutate_map_actors` is correct
- `mutate_data_table_rows` is correct
- `get_map_world_settings` should remain separate
- `get_data_table_schema` should remain separate

## `mutate_map_actors`

Registered in [WorldDataTools.cpp:1825](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WorldDataTools.cpp:1825).

### Tool Shape

Required:

- `map_path`
- `operations_json`

Optional:

- `save`
- `restore_previous_map`
- `max_depth`

### Operation Schema

`operations_json` is a JSON array.

Each item must contain:

- `op`

Supported `op` values:

- `spawn`
- `update`
- `remove`

### `spawn`

Fields:

- `op = "spawn"`
- `actor_class`
- optional `transform_json`
- optional `actor_label`
- optional `folder_path`
- optional `select_actor`

Example:

```json
[
  {
    "op": "spawn",
    "actor_class": "/Game/Foo/BP_MyActor.BP_MyActor_C",
    "actor_label": "DemoActor",
    "folder_path": "Demo/Sharpness",
    "transform_json": {
      "location": { "x": 0, "y": 0, "z": 100 },
      "rotation": { "pitch": 0, "yaw": 45, "roll": 0 },
      "scale": { "x": 1, "y": 1, "z": 1 }
    }
  }
]
```

### `update`

Fields:

- `op = "update"`
- `actor_identifier`
- optional `transform_json`
- optional `values_json`
- optional `actor_label`
- optional `folder_path`
- optional `select_actor`

Example:

```json
[
  {
    "op": "update",
    "actor_identifier": "DemoActor",
    "transform_json": {
      "location": { "x": 300, "y": 0, "z": 120 }
    },
    "values_json": {
      "SomeComponent.Intensity": { "kind": "float", "value": 5.0 },
      "bHidden": { "kind": "bool", "value": false }
    }
  }
]
```

### `remove`

Fields:

- `op = "remove"`
- `actor_identifier`

Example:

```json
[
  {
    "op": "remove",
    "actor_identifier": "DemoActor"
  }
]
```

### Batch Example

```json
[
  {
    "op": "spawn",
    "actor_class": "/Game/Foo/BP_DemoDriver.BP_DemoDriver_C",
    "actor_label": "SharpnessDriver"
  },
  {
    "op": "update",
    "actor_identifier": "SharpnessDriver",
    "folder_path": "Demo/Drivers"
  }
]
```

### Response Expectations

The tool returns:

- `success`
- `mutated_count`
- `failed_count`
- `saved`
- optional restore/save errors
- `results`

Each result item contains:

- `op`
- `success`
- operation-specific payload such as `actor` or `removed_actor`

## `mutate_data_table_rows`

Registered in [WorldDataTools.cpp:1840](/Users/bofa1ex/RiderProjects/2DParallaxScrolling_5.4/Plugins/VibeUE/Source/VibeUE/Private/Tools/WorldDataTools.cpp:1840).

### Tool Shape

Required:

- `table_path`

One of:

- `operations_json`
- `rows_json`

Optional:

- `save`
- `max_depth`

### Why Both `operations_json` and `rows_json` Exist

`rows_json` exists as a compatibility shorthand for the most common case: pure row upsert.

`operations_json` is the canonical mutation surface.

Use:

- `rows_json` for quick bulk upsert
- `operations_json` for mixed `upsert/delete/rename`

### Operation Schema

`operations_json` is a JSON array.

Each item must contain:

- `op`

Supported `op` values:

- `upsert`
- `delete`
- `rename`

### `upsert`

Fields:

- `op = "upsert"`
- `row_name`
- `row` or `value`

Example:

```json
[
  {
    "op": "upsert",
    "row_name": "Bow_01",
    "row": {
      "WeaponId": { "kind": "name", "value": "Bow_01" },
      "Label": { "kind": "string", "value": "Bow" },
      "ProfileAssetPath": {
        "kind": "soft_object",
        "object_path": "/Game/ProjectUI/.../DA_HPI_WeaponSharpnessProfile_Bow_01.DA_HPI_WeaponSharpnessProfile_Bow_01"
      }
    }
  }
]
```

### `delete`

Fields:

- `op = "delete"`
- `row_name`

Example:

```json
[
  {
    "op": "delete",
    "row_name": "Bow_01"
  }
]
```

### `rename`

Fields:

- `op = "rename"`
- `row_name`
- `new_row_name`

Example:

```json
[
  {
    "op": "rename",
    "row_name": "Bow_01",
    "new_row_name": "Bow_Primary_01"
  }
]
```

### Upsert Shorthand Example

```json
{
  "Bow_01": {
    "WeaponId": { "kind": "name", "value": "Bow_01" },
    "Label": { "kind": "string", "value": "Bow" }
  },
  "OneHandSword_01": {
    "WeaponId": { "kind": "name", "value": "OneHandSword_01" },
    "Label": { "kind": "string", "value": "Sword" }
  }
}
```

### Response Expectations

The tool returns:

- `success`
- `mutated_count`
- `failed_count`
- `saved`
- `results`

Each result item contains:

- `op`
- `row_name`
- `success`
- optional `row`
- optional `error`

## Decision Guidance for Agents

### Prefer Mutation Tools When

- several operations target the same map or table
- you want one save boundary
- you want one restore boundary
- you want one result envelope

### Prefer Separate Read Tools When

- you only need state inspection
- you need schema before mutation
- you want to keep token usage bounded

Examples:

- use `get_data_table_schema` before `create_data_table_with_row_struct_object`
- use `get_map_world_settings` before `set_map_world_settings`
- use `list_map_actors` before `mutate_map_actors` when actor identity is ambiguous

## Non-Goal

This document is not a complete end-user manual for every VibeUE tool.

It exists to make the mutation-tool pattern explicit so future tool additions do not regress back into many narrow micro-tools for the same resource family.
