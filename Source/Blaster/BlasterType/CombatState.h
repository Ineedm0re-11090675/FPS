#pragma once

UENUM(Blueprintable)
enum class ECombatState : uint8
{
	ECS_Unoccupied UMETA(DisplayName = "Unoccupied"),
	ECS_Reloading UMETA(DisplayName = "Reloading"),
	ECS_Throwing UMETA(DisplayName = "Throwing"),
	ECS_Swaping UMETA(DisplayName = "Swapping"),
	
	ECS_MAX UMETA(DisplayName = "DefaultMax")
};
