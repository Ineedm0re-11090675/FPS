# VibeUE Tool Retention Matrix

## Question

Do we really need all 33 currently exposed MCP tools?

Short answer:

- no, not all 33 are equally necessary
- yes, a large majority are justified
- a small set should be treated as optional or internal candidates

This document is about exposure policy, not implementation correctness.

## Current Count

Current public MCP tool count: `33`

High-level grouping:

- context: 7
- python: 2
- reflection: 3
- world/data: 8
- widget and widget-export: 8
- misc/specialized: 5

## Evaluation Criteria

A tool should remain public if most of these are true:

1. it represents a distinct user intent
2. it is likely to be called directly by an AI agent without hidden internal context
3. collapsing it into another tool would make the input contract much less clear
4. it is part of a common Unreal authoring workflow
5. it is not just plugin self-debugging or operator maintenance

A tool should be considered for internal-only or optional packaging if most of these are true:

1. it is mostly for plugin debugging
2. it is not specific to Unreal authoring workflows
3. it is rarely the first or second tool a caller should pick
4. its capability is already covered by a richer public tool or by a safe escape hatch

## Keep Public: Core Authoring Surface

These are justified as public tools.

### Context

- `context_overview`
- `editor_context`
- `project_info`
- `current_level`
- `selected_assets`
- `open_editors`

Reason:

- these are grounding tools
- they are read-only
- they represent distinct slices with different token costs

Note:

- `context_overview` should be the preferred first call
- the narrower tools remain valuable after the overview

### Python

- `execute_python_code`
- `discover_python_api`

Reason:

- one is the escape hatch
- one is the discovery surface

This is the right shape. More than two is unnecessary here.

### Reflection

- `get_object_schema`
- `get_object_properties`
- `set_object_properties`

Reason:

- these are foundational typed primitives
- many higher-level workflows depend on them

### World and Data

- `get_map_world_settings`
- `set_map_world_settings`
- `list_map_actors`
- `mutate_map_actors`
- `get_data_table_rows`
- `mutate_data_table_rows`
- `create_data_table_with_row_struct_object`
- `get_data_table_schema`

Reason:

- these cover two of the highest-frequency structured-authoring resources
- the mutation tools are already correctly consolidated by resource family

### Widget Authoring

- `widget_blueprint`
- `widget_designer`

Reason:

- these are direct authoring tools
- they are distinct from export/review helpers

## Keep Public: Specialized but Legitimate

These are not foundational, but they are still valid public tools.

- `export_widget_logic_summary`
- `export_widget_logic_summary_batch`
- `export_widget_spec`
- `export_widget_spec_batch`
- `validate_widget_spec`
- `export_widget_review`
- `read_logs`
- `import_texture`
- `niagara_system`

Reason:

- all of them support real content-authoring or debugging workflows
- they are not just plugin-maintenance tools

However, they are not all equally shaped ideally.

## Candidates for Consolidation

These do not necessarily need to disappear, but they are the clearest next consolidation targets.

### Widget export pairs

- `export_widget_logic_summary`
- `export_widget_logic_summary_batch`

Recommendation:

- eventually collapse into one tool with `batch=true` or `package_path`

### Widget spec/export pairs

- `export_widget_spec`
- `export_widget_spec_batch`

Recommendation:

- eventually collapse into one tool with `batch=true` or `package_path`

Reason:

- these are the same resource family
- same operation class
- same result family
- only cardinality differs

## Candidates for Internal-Only or Optional Packaging

These are the least necessary as always-public MCP tools.

- `self_test`
- `manage_skills`
- `deep_research`

### `self_test`

Why candidate:

- this is mostly plugin/operator health debugging
- `context_overview` and `editor_context` already expose a lot of live health signal

Recommendation:

- consider making it internal-only, or at least lower-priority in discovery UIs

### `manage_skills`

Why candidate:

- this is not really Unreal authoring
- it is meta-tooling for the assistant/runtime

Recommendation:

- move toward internal-only unless VibeUE explicitly wants to be a general Codex skill-management host

### `deep_research`

Why candidate:

- it is useful, but not specifically part of Unreal editor automation
- it feels more like a platform-level assistant capability than a VibeUE-native authoring primitive

Recommendation:

- consider optional packaging or a separate extension category instead of keeping it in the core VibeUE public set

## Net Recommendation

### Public and stable

Keep these as the stable public authoring surface:

- context tools except `self_test`
- python tools
- reflection tools
- world/data tools
- widget authoring tools
- widget export/review tools
- logs / texture import / niagara

### Consider demoting

- `self_test`
- `manage_skills`
- `deep_research`

That would reduce the public count from `33` to `30` without harming real Unreal authoring workflows.

### Consider later consolidation

- widget batch vs single export pairs

That would reduce the public count further after API migration planning.

## Final Judgment

So, are all 33 needed?

- no, not as first-class public tools of equal status
- yes, roughly 30 are defensible today
- the best immediate cleanup is not deleting useful world/data/reflection tools
- the best cleanup is:
  - keep resource-family mutation tools consolidated
  - demote operator/meta tools
  - merge batch/single export pairs later
