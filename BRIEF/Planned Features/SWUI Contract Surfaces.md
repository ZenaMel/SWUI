# Roadmap Brief: SWUI Queries and Commands

## Goal

Extend SWUI beyond state + events by adding explicit support for callable functions while keeping each concept cleanly separated.

Current model:

| UE/AngelScript thing | Metadata | Generated section | Meaning |
|---|---|---|---|
| `UPROPERTY` value | `meta=(SwuiExpose)` | `state` | pushed/subscribed value |
| delegate property | `meta=(SwuiExpose)` | `events` | UE → JS event |
| zero-arg pure/const getter | `meta=(SwuiQuery)` or `meta=(SwuiComputed)` | `queries` / `computed` | callable read-only derived value |
| side-effect function | `meta=(SwuiCommand)` | `commands` | JS → UE action |

## Why

Avoid conflating different concepts:

```txt
CurrentState        = state source
IsTetherAttached()  = computed query
OnTetherAttached    = UE -> JS event
FireTether()        = JS -> UE command/action
````

## Example query

```angelscript
UFUNCTION(Category = "Tether", meta = (SwuiQuery))
bool IsTetherAttached() const
{
	return CurrentState == ETetherState::Attached || CurrentState == ETetherState::Pulling;
}
```

Generated TS idea:

```ts
const attached = await TetherComponent.IsTetherAttached();
```

Contract idea:

```ts
queries: {
	"tethercomponent.IsTetherAttached": {
		source: "TetherComponent",
		function: "IsTetherAttached",
		returnType: "boolean",
	},
}
```

## Example command

```angelscript
UFUNCTION(Category = "Tether", meta = (SwuiCommand))
void FireTether()
{
	// JS-callable action
}
```

Generated TS idea:

```ts
await TetherComponent.FireTether();
```

Contract idea:

```ts
commands: {
	"tethercomponent.FireTether": {
		source: "TetherComponent",
		function: "FireTether",
		payload: {},
	},
}
```

## Design rule

`SwuiExpose` stays for properties and delegates only.

Use separate metadata for callable functions:

```txt
SwuiExpose  = state + events
SwuiQuery   = read-only computed function
SwuiCommand = side-effect function/action
```

This keeps the API predictable and avoids mixing state, events, computed reads, and commands into one overloaded concept.
