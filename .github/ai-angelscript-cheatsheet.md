Agreed — the EnhancedInput stuff is API trivia. The AI only needs **language/convention traps**, not examples it can infer from C++.

Use this tighter version:

````
# Unreal Angelscript AI Cheatsheet

Audience: AI coding assistant.
Goal: write Unreal Engine Angelscript `.as` code that compiles.
Do not blindly translate C++.

## Core mindset

Unreal Angelscript looks like C++, but it is not C++.

Prefer:
- script-style Unreal syntax
- Blueprint-like names
- object references
- dot calls
- simplified Unreal macros
- AS function-library namespaces

Avoid:
- pointer syntax
- arrow calls
- C++ constructors
- raw C++ delegate macros
- assuming C++ members are exposed
- assuming `FMath::` exists

## Object references

AS does not use C++ pointer syntax for UObject refs.

Use:

```cpp
AActor Actor;
Actor.DoThing();

if (Actor != nullptr)
{
    Actor.DestroyActor();
}

````

Do not use:

```
AActor* Actor;
Actor->DoThing();

```

## Casts

Use `Cast<Type>(Value)`.

```
AActor OwnerActor = GetOwner();

APawn PawnOwner = Cast<APawn>(OwnerActor);
if (PawnOwner != nullptr)
{
    AController Controller = PawnOwner.Controller;
}

```

Avoid local names that shadow accessors/properties.

Prefer:

```
AActor OwnerActor = GetOwner();

```

Avoid:

```
AActor Owner = GetOwner();

```

## Properties

`UPROPERTY()` is simplified compared to C++.

Usually use:

```
UPROPERTY(Category = "Weapon|Config")
float FireRate = 10.0f;

```

Do not reflexively write:

```
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "...")

```

Internal state usually does not need `UPROPERTY()`.

```
private float Cooldown = 0.0f;

```

## Functions

`UFUNCTION()` is simplified.

Use:

```
UFUNCTION(Category = "Weapon")
void Reload()
{
}

```

Do not reflexively write:

```
UFUNCTION(BlueprintCallable)

```

Use `BlueprintOverride` for engine/Blueprint events:

```
UFUNCTION(BlueprintOverride)
void BeginPlay()
{
}

UFUNCTION(BlueprintOverride)
void Tick(float DeltaSeconds)
{
}

```

## Events vs delegates

This is a major trap.

Use `event` for multicast/event-dispatcher style `.Broadcast()`.

```
event void FWeaponFiredEvent();

UPROPERTY(Category = "Events")
FWeaponFiredEvent OnFired;

OnFired.Broadcast();

```

Use `delegate` for single-cast callable delegates.

```
delegate void FMyDelegate(int Value);

FMyDelegate Callback;
Callback.ExecuteIfBound(123);

```

Rule:

```
.Broadcast() => event
.ExecuteIfBound() => delegate

```

Do not write C++ delegate macros:

```
DECLARE_DYNAMIC_MULTICAST_DELEGATE(...)

```

## Function libraries

Do not assume C++ static library names.

Common AS style:

```
System::LineTraceSingle(...)
System::SetTimer(...)
Gameplay::GetPlayerController(...)
Math::Clamp(...)
Math::Min(...)
Math::Max(...)

```

Avoid:

```
FMath::Clamp(...)
UGameplayStatics::...
UKismetSystemLibrary::...

```

If `Math::Clamp`, `Math::Min`, or `Math::Max` are not available in the project bindings, use tiny local helpers instead of fighting the API.

```
private float ClampFloat(float Value, float MinValue, float MaxValue)
{
    if (Value < MinValue) return MinValue;
    if (Value > MaxValue) return MaxValue;
    return Value;
}

```

## Constructors and defaults

Do not write C++ constructors.

Use class-body defaults:

```
UPROPERTY(Category = "Config")
float Speed = 600.0f;

```

Use `default` only for exposed default-object/subobject style properties that are actually valid in AS.

```
default Mesh.bGenerateOverlapEvents = true;

```

Do not assume C++ tick setup compiles:

```
default PrimaryComponentTick.bCanEverTick = true;

```

If diagnostics say the member does not exist, remove it. Let the AS override exist, then enable ticking through supported project/Blueprint settings if needed.

## Components

Typical component declaration style:

```
UPROPERTY(DefaultComponent, RootComponent)
USceneComponent SceneRoot;

UPROPERTY(DefaultComponent, Attach = SceneRoot)
UStaticMeshComponent Mesh;

```

Do not create default subobjects like C++.

Avoid:

```
CreateDefaultSubobject<UStaticMeshComponent>(...)

```

## FName literals

Use `n"Name"` for name literals.

```
System::SetTimer(this, n"HandleTimer", 1.0f, false);

```

Bound functions should usually be `UFUNCTION()`.

```
UFUNCTION()
private void HandleTimer()
{
}

```

## Blueprint events

Use `BlueprintEvent` when Blueprints should override behavior.

```
UFUNCTION(BlueprintEvent)
void BP_OnFired()
{
}

```

Good pattern:

```
void Fire()
{
    // script logic
    BP_OnFired();
}

UFUNCTION(BlueprintEvent, DisplayName = "On Fired")
void BP_OnFired()
{
}

```

## Script mixins

Use mixins when AS needs methods that should feel native on a type.

AS mixin:

```
mixin void ResetToZero(FVector& Value)
{
    Value = FVector(0, 0, 0);
}

```

C++ mixin exposure pattern:

```
UCLASS(Meta = (ScriptMixin = "FVector"))
class UVectorScriptMixinLibrary : public UObject
{
    GENERATED_BODY()

public:
    UFUNCTION(ScriptCallable)
    static float GetMagnitude2D(const FVector& Value)
    {
        return FVector(Value.X, Value.Y, 0.0f).Length();
    }
};

```

AS usage:

```
float M = Vector.GetMagnitude2D();

```

Rules:

* `ScriptMixin = "TargetType"`
* static function
* first parameter is receiver
* `UFUNCTION(ScriptCallable)`
* AS calls it like an instance method

## Common C++ to AS translations

| C++                                   | Angelscript                                  |
| ------------------------------------- | -------------------------------------------- |
| AActor\* Actor                        | AActor Actor                                 |
| Actor->Foo()                          | Actor.Foo()                                  |
| if (Actor)                            | if (Actor !\= nullptr)                       |
| FMath::Clamp(...)                     | Math::Clamp(...) or helper                   |
| UGameplayStatics::...                 | Gameplay::...                                |
| UKismetSystemLibrary::...             | System::...                                  |
| DECLARE_DYNAMIC_DELEGATE            | delegate void FName(...)                     |
| DECLARE_DYNAMIC_MULTICAST_DELEGATE | event void FName(...)                        |
| constructor defaults                  | class-body values / valid default statements |
| BlueprintCallable                     | usually just UFUNCTION()                     |
| EditAnywhere, BlueprintReadWrite      | usually just UPROPERTY()                     |
| ReceiveBeginPlay                      | BeginPlay with BlueprintOverride             |
| ->                                    | .                                            |
| \* pointer type                       | plain object ref                             |

## Diagnostic repair rules

When user gives AS compiler errors:

1. Trust diagnostics over C++ instinct.
2. Remove unsupported C++ members.
3. Replace pointer/arrow syntax.
4. Replace `FMath::` with `Math::` or local helpers.
5. Use `event` for `.Broadcast()`.
6. Use `delegate` only for single-cast callbacks.
7. Rename shadowing locals like `Owner` to `OwnerActor`.
8. Initialize variables explicitly to silence AS warnings.
9. Prefer minimal patch.
10. Do not introduce unrelated Unreal C++ architecture.

## Output rules for AI

When fixing AS code:

* Give compile-error cause first.
* Give exact replacement/diff.
* Use Angelscript syntax, not C++.
* Do not add API-specific examples unless user asks.
* Do not explain obvious Unreal concepts.
* Prefer small, compiling code over clever code.

````

For `.github/copilot-instructions.md`, I’d append only this compact block, not the whole long sheet:

```md
## Unreal Angelscript rules

For `.as` files, do not blindly write C++.

- No pointers: use `AActor Actor`, not `AActor* Actor`.
- No arrows: use `Actor.Foo()`, not `Actor->Foo()`.
- Null check with `Obj != nullptr`.
- Use `Cast<Type>(Obj)`.
- Use `UPROPERTY()` / `UFUNCTION()` simplified defaults.
- Use `UFUNCTION(BlueprintOverride)` for `BeginPlay`, `Tick`, etc.
- Use `event void FName(...)` for multicast `.Broadcast()`.
- Use `delegate void FName(...)` for single-cast callbacks.
- Do not use C++ delegate macros.
- Prefer `System::`, `Gameplay::`, `Math::` over C++ library names.
- Do not assume `FMath::` exists.
- Do not write C++ constructors or `CreateDefaultSubobject`.
- Do not assume C++ members like `PrimaryComponentTick.bCanEverTick` are exposed.
- Use local helpers for clamp/min/max if bindings complain.
- Trust AS diagnostics over C++ habits.

````