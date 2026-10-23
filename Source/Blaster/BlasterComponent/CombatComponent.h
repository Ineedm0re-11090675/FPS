// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/BlasterType/CombatState.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "UObject/CoreNetTypes.h"
#include "CombatComponent.generated.h"

class AWeapon;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class BLASTER_API UCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCombatComponent();
	//这样Character可以完全访问
	friend class AMyBlasterCharacter;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	void EquipPrimaryWeapon(AWeapon* EquipWeapon);
	void EquipSecondaryWeapon(AWeapon* EquipWeapon);
	void EquipWeapon(AWeapon* WeaponToEquipped);
	void SwapWeapon();
	bool ShouldSwap();
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
	UFUNCTION()
	void OnRep_EquippedWeapon();
	UFUNCTION()
	void OnRep_PrimaryWeapon();
	UFUNCTION()
	void OnRep_SecondaryWeapon();
	void Reload();
	//做一个回调函数，动画结束之后就可以改State
	UFUNCTION(BlueprintCallable)
	void FinishReload();

	UFUNCTION(BlueprintCallable)
	void FinishSwaping();
	UFUNCTION(Server, Reliable)
	void ServerFinishSwaping();

	UFUNCTION(BlueprintCallable)
	void FinishSwapAttachWeapon();
	UFUNCTION(Server, Reliable)
	void ServerFinishSwapAttachWeapon();
		

	UFUNCTION(BlueprintCallable)
	void ThrowingGrenadeFinish();
	UFUNCTION(Server, Reliable)
	void ServerThrowGrenadeFinish();

	UFUNCTION(BlueprintCallable)
	void LaunchGrenade();
	UFUNCTION(Server, Reliable)
	void ServerLaunchGrenade(const FVector_NetQuantize& TraceHitTarget);

	void SpawnGrenade(const FVector_NetQuantize& TraceHitTarget);

	UFUNCTION(BlueprintCallable)
	void ShotGunShellReload();

	void JumpToShotGunEnd();
	//为了让controller调用
	void FireButtonPressed(bool Pressed);
	void BroadcastImpactEffect(class UNiagaraSystem* ImpactEffect, class USoundCue* ImpactSound,
	                           const FVector& ImpactLocation,
	                           const FVector& ImpactNormal);
	void PickUpAmmo(EWeaponType WeaponType, int32 AmmoAmount);

protected:
	virtual void BeginPlay() override;
	void SetAiming(bool bAiming);

	void LocalFire(const FVector_NetQuantize& TraceHitTarget);

	void ShotGunLocalFire(const TArray<FVector_NetQuantize>& TraceHitTargets);
	UFUNCTION(Server, Reliable)
	void ServerSetAiming(bool bAiming);

	UFUNCTION(Server, Reliable,WithValidation)
	void ServerFire(const FVector_NetQuantize& TraceHitTarget ,float FireDelay);
	//FVector_NetQuantize是一种为了节约宽带的净向量，没有小数，向下取整
	UFUNCTION(NetMulticast, Reliable)
	void MulticastFire(const FVector_NetQuantize& TraceHitTarget);

	UFUNCTION(Server, Reliable,WithValidation)
	void ServerShotGunFire(const TArray<FVector_NetQuantize>& TraceHitTargets,float FireDelay);
	//FVector_NetQuantize是一种为了节约宽带的净向量，没有小数，向下取整
	UFUNCTION(NetMulticast, Reliable)
	void MulticastShotGunFire(const TArray<FVector_NetQuantize>& TraceHitTargets);
	

	UFUNCTION(NetMulticast, Reliable)
	void MulticastImpactEffect(class UNiagaraSystem* ImpactEffect, class USoundCue* ImpactSound,
	                           const FVector_NetQuantize& ImpactLocation,
	                           const FVector_NetQuantizeNormal& ImpactNormal);

	void TraceUnderCrosshairs(FHitResult& TraceHitResult, FVector& TraceHitTarget);

	void SetHUDCrosshair(float DeltaTime);

	UFUNCTION(Server, Reliable)
	void ServerReload();

	void HandleReload();

	int32 AmountToReload();

	void UpdateAmmoValues();
	void UpdateShotGunAmmoValues();


	void UpdateGrenadeAmount();
	void ShowGrenade(bool bShouldShow);
	void ThrowGrenade();
	UFUNCTION(Server, Reliable)
	void ServerThrowGrenade();

	void EquipWeaponInHand(AWeapon* WeaponToEquip);
	void StowWeapon(AWeapon* WeaponToStow);
	void UpdateCarriedAmmoForEquippedWeapon();
	void UpdateWeaponHUD();
	bool IsPistolWeapon(AWeapon* Weapon) const;

	void AttachWeaponToLHand(AActor* ActorToAttach);
	void AttachWeaponToRHand();
	void AttachWeaponToBackPack(AActor* ActorToAttach);
	UPROPERTY(EditAnywhere)
	TSubclassOf<class AMyProjectile> GrenadeClass;
	void ProjectileFire();
	void HitScanFire();
	void ShotGunFire();

private:
	UPROPERTY()
	class AMyBlasterCharacter* Character;
	UPROPERTY()
	class ABlasterPlayerController* Controller;
	UPROPERTY()
	class ABlasterHUD* HUD;

	void UpdateCharacterMovementSpeed();

	UPROPERTY(ReplicatedUsing=OnRep_EquippedWeapon)
	AWeapon* EquippedWeapon;
	UPROPERTY(ReplicatedUsing=OnRep_PrimaryWeapon)
	AWeapon* PrimaryWeapon;
	UPROPERTY(ReplicatedUsing=OnRep_SecondaryWeapon)
	AWeapon* SecondaryWeapon;


	UPROPERTY(ReplicatedUsing= OnRep_Aiming)
	bool bIsAiming = false;
	UFUNCTION()
	void OnRep_Aiming();

	bool bLocallyReload = false;
	bool bAimingPressed =false;
	UPROPERTY(EditAnywhere)
	float BaseWalkSpeed;
	UPROPERTY(EditAnywhere)
	float AimingSpeed;

	bool bFireButtonPressed = false;

	//
	//Tackle crosshair spread
	//
	float CrosshairVelocityFactor;
	float CrosshairInAirFactor;
	float CrosshairAimFactor;
	float CrosshairShootingFactor;
	FHUDPackage HudPackage;
	//
	//wait to be improving
	//
	FVector HitTarget;

	//FOV
	float DefaultFOV;
	float CurrentFOV;
	UPROPERTY(EditAnywhere, Category = "Combat")
	float ZoomedFOV = 30.f;
	UPROPERTY(EditAnywhere, Category = "Combat")
	float ZoomedInterpSpeed = 10.f;

	void InterpFOV(float DeltaTime);

	//
	//Automatic Fire
	//
	FTimerHandle FireTimer;
	bool bCanFire = true;
	void StartTimer();
	void FinishTimer();
	void Fire();
	static FVector ResolveTraceHitTarget(const FHitResult& TraceHitResult, const FVector& TraceEnd);
	static bool IsCrosshairTargetActor(AActor* HitActor);
	bool CanFire() const;

	//Carried Ammo For the Current Weapon
	UPROPERTY(ReplicatedUsing=OnRep_CarriedAmmo)
	int32 CarriedAmmo;

	UFUNCTION()
	void OnRep_CarriedAmmo();
	//TMap不能被复制
	TMap<EWeaponType, int32> CarriedAmmoMap;

	void InitializeCarriedAmmo();
	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingARAmmo = 60;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingRocketAmmo = 1;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingPistolAmmo = 30;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingSubmachineAmmo = 32;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingShotGunAmmo = 10;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingSniperRifleAmmo = 10;

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 StartingGrenadeLauncherAmmo = 3;

	UPROPERTY(ReplicatedUsing= OnRep_Grenade)
	int32 GrenadeAmount = 4;

	UFUNCTION()
	void OnRep_Grenade();

	UPROPERTY(EditAnywhere, Category = "InitAmmo")
	int32 MaxGrenadeAmount = 4;

	UFUNCTION()
	void OnRep_CombatState();
	UPROPERTY(ReplicatedUsing= OnRep_CombatState)
	ECombatState CombatState = ECombatState::ECS_Unoccupied;
	bool bSwapAttachCompleted = false;


	/*
	 *大狗叫！！！！！
	 */
	bool bIsRocketCharging = false;

	FTimerHandle RocketChargeTimer;
	float RocketChargeStartedTime = 0.f;
	float RocketChargeStartFOV = 0.f;
	UPROPERTY()
	UAudioComponent* RocketChargeAudioComponent = nullptr;

	bool ShouldUseRocketChargeFire();

	void BeginRocketCharge();
	void CancelRocketCharge();
	void FinishRocketCharge();


#if WITH_DEV_AUTOMATION_TESTS
	friend class FCombatComponentTraceTargetTest;
	friend class FCombatComponentCrosshairTargetTest;
#endif

public:
	void SetGrenadeAmount(int32 Amount);
	FORCEINLINE int32 GetGrenadeAmount() const { return GrenadeAmount; }
};
