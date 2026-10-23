#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BuffComponent.generated.h"
class AMyBlasterCharacter;

UCLASS(ClassGroup=(Custom),meta = (BlueprintSpawnableComponent))
class BLASTER_API UBuffComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UBuffComponent();
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	friend class AMyBlasterCharacter;
	void Heal(float HealAmount,float HealTime);
	void HealRampUp(float DeltaTime);
	void ReplenishShield(float ShieldAmount, float ShieldTime);
	void ShieldRampUp(float DeltaTime);
	void BuffSpeed(float BaseSpeedBuff,float CrouchSpeedBuff,float SpeedBuffTime);
	void BuffJump(float JumpVelocity,float JumpBuffTime);
	void SetInitialSpeed(float BaseSpeed,float CrouchSpeed);
	void SetInitialJumpSpeed(float JumpSpeed);
	
	void ApplySpeed(float BaseSpeed, float CrouchSpeed);
	float GetCurrentBaseSpeed() const;
	float GetCurrentCrouchSpeed() const;
private:
	UPROPERTY()
	AMyBlasterCharacter* Character;
	bool bHealing =false;
	float HealingAmount = 0.f;
	float HealingRate =0.f;


	bool bShielding =false;
	float ShieldingAmount = 0.f;
	float ShieldingRate =0.f;
	/*
	 *Speed Static
	 */
	FTimerHandle SpeedTimerHandle;
	void ResetSpeed();
	float InitialBaseSpeed = 0.f;
	float InitialCrouchSpeed = 0.f;
		
	bool bSpeedBuffActive = false;
	float BuffedBaseSpeed = 0.f;
	float BuffedCrouchSpeed = 0.f;

	UFUNCTION(NetMulticast,Reliable)
	void MulticastSpeedBuff(float BaseBuffSpeed,float CrouchBuffSpeed,bool bBuffActive);
	/*
	 *Jump
	 */
	FTimerHandle JumpTimerHandle;
	void ResetJump();
	float InitialJumpSpeed = 0.f;
	
	UFUNCTION(NetMulticast,Reliable)
	void MulticastJumpBuff(float JumpVelocity);
protected:
	virtual void BeginPlay() override;
};
