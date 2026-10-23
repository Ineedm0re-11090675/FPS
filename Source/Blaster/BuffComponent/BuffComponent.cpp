#include "BuffComponent.h"

#include "Blaster/Character/MyBlasterCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"

UBuffComponent::UBuffComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UBuffComponent::ApplySpeed(float BaseSpeed, float CrouchSpeed)
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;

	Character->GetCharacterMovement()->MaxWalkSpeed = BaseSpeed;
	Character->GetCharacterMovement()->MaxWalkSpeedCrouched = CrouchSpeed;
}

void UBuffComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UBuffComponent::BuffJump(float JumpVelocity, float JumpBuffTime)
{
	if (Character == nullptr || Character->IsElimmed()) return;
	if (Character->GetCharacterMovement() == nullptr) return;
	if (JumpVelocity <= 0.f || JumpBuffTime <= 0.f) return;
	
	Character->GetWorldTimerManager().SetTimer(
		JumpTimerHandle,
		this,
		&UBuffComponent::ResetJump,
		JumpBuffTime
	);
	Character->GetCharacterMovement()->JumpZVelocity = JumpVelocity;
	MulticastJumpBuff(JumpVelocity);
}

void UBuffComponent::BuffSpeed(float BaseSpeedBuff, float CrouchSpeedBuff, float SpeedBuffTime)
{
	if (Character == nullptr || Character->IsElimmed()) return;
	if (Character->GetCharacterMovement() == nullptr) return;
	if (BaseSpeedBuff <= 0.f || CrouchSpeedBuff <= 0.f || SpeedBuffTime <= 0.f) return;
		
	bSpeedBuffActive = true;
	BuffedBaseSpeed = BaseSpeedBuff;
	BuffedCrouchSpeed = CrouchSpeedBuff;
	
	Character->GetWorldTimerManager().SetTimer(
		SpeedTimerHandle,
		this,
		&UBuffComponent::ResetSpeed,
		SpeedBuffTime
	);
	MulticastSpeedBuff(BaseSpeedBuff, CrouchSpeedBuff, true);
}

float UBuffComponent::GetCurrentBaseSpeed() const
{
	return bSpeedBuffActive ? BuffedBaseSpeed : InitialBaseSpeed;
}

float UBuffComponent::GetCurrentCrouchSpeed() const
{
	return bSpeedBuffActive ? BuffedCrouchSpeed : InitialCrouchSpeed;
}

void UBuffComponent::Heal(float HealAmount, float HealTime)
{
	if (Character == nullptr || Character->IsElimmed()) return;
	if (HealAmount <= 0.f || HealTime <= 0.f) return;
	
	bHealing = true;
	HealingAmount += HealAmount;
	HealingRate = HealAmount / HealTime;
}

void UBuffComponent::HealRampUp(float DeltaTime)
{
	if (!bHealing || Character == nullptr || Character->IsElimmed()) return;
	const float HealThisFrame = DeltaTime * HealingRate;
	const float NewHealth = FMath::Clamp(Character->GetHealth() + HealThisFrame, 0.f, Character->GetMaxHealth());
	Character->SetHealth(NewHealth);
	Character->UpdateHUDHealth();
	HealingAmount -= HealThisFrame;
	if (HealingAmount <= 0.f || Character->GetHealth() >= Character->GetMaxHealth())
	{
		bHealing = false;
		HealingAmount = 0.f;
	}
}

void UBuffComponent::MulticastJumpBuff_Implementation(float JumpVelocity)
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;
	Character->GetCharacterMovement()->JumpZVelocity = JumpVelocity;
}

void UBuffComponent::MulticastSpeedBuff_Implementation(float BaseBuffSpeed, float CrouchBuffSpeed, bool bBuffActive)
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;

	bSpeedBuffActive = bBuffActive;
	if (bSpeedBuffActive)
	{
		BuffedBaseSpeed = BaseBuffSpeed;
		BuffedCrouchSpeed = CrouchBuffSpeed;
	}
	else
	{
		BuffedBaseSpeed = 0.f;
		BuffedCrouchSpeed = 0.f;
	}

	const float TargetBaseSpeed = Character->IsAiming()
		? Character->GetCharacterMovement()->MaxWalkSpeed
		: BaseBuffSpeed;

	ApplySpeed(TargetBaseSpeed, CrouchBuffSpeed);
}

void UBuffComponent::ReplenishShield(float ShieldAmount, float ShieldTime)
{
	if (Character == nullptr || Character->IsElimmed()) return;
	if (ShieldAmount <= 0.f || ShieldTime <= 0.f) return;
	
	bShielding = true;
	ShieldingAmount += ShieldAmount;
	ShieldingRate = ShieldAmount / ShieldTime;
}

void UBuffComponent::ResetJump()
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;
	Character->GetCharacterMovement()->JumpZVelocity = InitialJumpSpeed;
	MulticastJumpBuff(InitialJumpSpeed);
}

void UBuffComponent::ResetSpeed()
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;
	bSpeedBuffActive = false;
	MulticastSpeedBuff(InitialBaseSpeed, InitialCrouchSpeed, false);
}

void UBuffComponent::SetInitialJumpSpeed(float JumpSpeed)
{
	InitialJumpSpeed = JumpSpeed; 
}

void UBuffComponent::SetInitialSpeed(float BaseSpeed, float CrouchSpeed)
{
	InitialBaseSpeed = BaseSpeed;
	InitialCrouchSpeed = CrouchSpeed;
}

void UBuffComponent::ShieldRampUp(float DeltaTime)
{
	if (!bShielding || Character == nullptr || Character->IsElimmed()) return;
	const float ShieldThisFrame = DeltaTime * ShieldingRate;
	const float NewShield = FMath::Clamp(Character->GetShield() + ShieldThisFrame, 0.f, Character->GetMaxShield());
	Character->SetShield(NewShield);
	Character->UpdateHUDShield();
	ShieldingAmount -= ShieldThisFrame;
	if (ShieldingAmount <= 0.f || Character->GetShield() >= Character->GetMaxShield())
	{
		bShielding = false;
		ShieldingAmount = 0.f;
	}
}

void UBuffComponent::TickComponent(float DeltaTime, enum ELevelTick TickType,
                                   FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	HealRampUp(DeltaTime);
	ShieldRampUp(DeltaTime);
}
