#if WITH_DEV_AUTOMATION_TESTS

#include "Blaster/BlasterType/CombatState.h"
#include "Blaster/Character/BlasterAnimInstance.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlasterAnimInstanceWeaponAimingTest,
	"Blaster.Animation.WeaponAiming.DisabledByGameplayState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FBlasterAnimInstanceWeaponAimingTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Weapon aiming is enabled during normal unoccupied gameplay"),
		UBlasterAnimInstance::ShouldUseWeaponAimingForAnimation(
			false,
			false,
			ECombatState::ECS_Unoccupied
		)
	);
	TestFalse(
		TEXT("Weapon aiming is disabled when gameplay is disabled"),
		UBlasterAnimInstance::ShouldUseWeaponAimingForAnimation(
			true,
			false,
			ECombatState::ECS_Unoccupied
		)
	);
	TestFalse(
		TEXT("Weapon aiming is disabled after elimination"),
		UBlasterAnimInstance::ShouldUseWeaponAimingForAnimation(
			false,
			true,
			ECombatState::ECS_Unoccupied
		)
	);
	TestFalse(
		TEXT("Weapon aiming is disabled while reloading"),
		UBlasterAnimInstance::ShouldUseWeaponAimingForAnimation(
			false,
			false,
			ECombatState::ECS_Reloading
		)
	);

	return true;
}

#endif
