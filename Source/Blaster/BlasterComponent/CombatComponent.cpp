// Fill out your copyright notice in the Description page of Project Settings.


#include "CombatComponent.h"
#include "../Weapon/Weapon.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BuffComponent/BuffComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/SkeletalMeshSocket.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundCue.h"
#include "../Weapon/MyProjectile.h"
#include "Blaster/Weapon/ShotGun.h"

UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	BaseWalkSpeed = 600.f;
	AimingSpeed = 450.f;
}

void UCombatComponent::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UCombatComponent, EquippedWeapon);
	DOREPLIFETIME(UCombatComponent, PrimaryWeapon);
	DOREPLIFETIME(UCombatComponent, bIsAiming);
	DOREPLIFETIME_CONDITION(UCombatComponent, CarriedAmmo, COND_OwnerOnly);
	DOREPLIFETIME(UCombatComponent, CombatState);
	DOREPLIFETIME(UCombatComponent, GrenadeAmount);
	DOREPLIFETIME(UCombatComponent, SecondaryWeapon);
}

void UCombatComponent::BeginPlay()
{
	Super::BeginPlay();

	if (Character)
	{
		Character->GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed;
		//
		//初始化FOV
		//
		if (Character->GetCharacterMovement())
		{
			DefaultFOV = Character->GetFollowCamera()->FieldOfView;
			CurrentFOV = DefaultFOV;
		}
	}
	//初始化 原始携带子弹数量
	if (Character && Character->HasAuthority())
	{
		InitializeCarriedAmmo();
	}
}

void UCombatComponent::InterpFOV(float DeltaTime)
{
	if (EquippedWeapon == nullptr) return;
	if (bIsRocketCharging)
	{
		const float ChargeDuration = FMath::Max(EquippedWeapon->ChargeDuration, 0.01f);
		const float ElapsedTime = GetWorld()->GetTimeSeconds() - RocketChargeStartedTime;
		const float Alpha = FMath::Clamp(ElapsedTime / ChargeDuration, 0.f, 1.f);
		const float SmoothAlpha = FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f);
		CurrentFOV = FMath::Lerp(
			RocketChargeStartFOV,
			EquippedWeapon->ChargeMinFOV,
			SmoothAlpha
		);
	}
	else if (bIsAiming)
	{
			CurrentFOV = FMath::FInterpTo(CurrentFOV, EquippedWeapon->GetZoomedFOV(), DeltaTime,
			                              EquippedWeapon->GetZoomedSpeed());
	}
	else
	{
		CurrentFOV = FMath::FInterpTo(CurrentFOV, DefaultFOV, DeltaTime, ZoomedInterpSpeed);
	}
	//
	//以上计算FOV，以下设置FOV
	//
	if (Character && Character->GetFollowCamera())
	{
		Character->GetFollowCamera()->SetFieldOfView(CurrentFOV);
	}
}

void UCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                     FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	//不会每帧都去计算LineTrace，这样性能不好，在fire的那一帧去做就行了
	//
	//Attention:我认为这里性能很差，为了一个手正确用了tick，可改
	//只让本地去traceline
	//
	if (Character && Character->IsLocallyControlled())
	{
		SetHUDCrosshair(DeltaTime);

		FHitResult HitResult;
		FVector TraceHitTarget = HitTarget;
		TraceUnderCrosshairs(HitResult, TraceHitTarget);
		HitTarget = TraceHitTarget;

		//
		//FOV
		//
		InterpFOV(DeltaTime);
	}
}

void UCombatComponent::SetHUDCrosshair(float DeltaTime)
{
	if (Character == nullptr || Character->Controller == nullptr)return;
	//从角色处拿
	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		HUD = HUD == nullptr ? Cast<ABlasterHUD>(Controller->GetHUD()) : HUD;
		if (HUD)
		{
			if (EquippedWeapon)
			{
				HudPackage.CrosshairCenter = EquippedWeapon->CrossHairCenter;
				HudPackage.CrosshairRight = EquippedWeapon->CrossHairRight;
				HudPackage.CrosshairLeft = EquippedWeapon->CrossHairLeft;
				HudPackage.CrosshairUp = EquippedWeapon->CrossHairTop;
				HudPackage.CrosshairBottom = EquippedWeapon->CrossHairButton;
			}
			else
			{
				HudPackage.CrosshairCenter = nullptr;
				HudPackage.CrosshairRight = nullptr;
				HudPackage.CrosshairLeft = nullptr;
				HudPackage.CrosshairUp = nullptr;
				HudPackage.CrosshairBottom = nullptr;
			}
			//Calculate crosshair spread
			//
			//1.CharacterSpeed
			//[0,600]->[0,1]
			FVector2D WalkSpeedRange(0.f, Character->GetCharacterMovement()->MaxWalkSpeed);
			FVector2D VelocityRange(0.f, 1.f);
			FVector Velocity = Character->GetVelocity();
			Velocity.Z = 0.f;
			CrosshairVelocityFactor = FMath::GetMappedRangeValueClamped(WalkSpeedRange, VelocityRange, Velocity.Size());


			if (Character->GetCharacterMovement()->IsFalling())
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor, 2.f, DeltaTime, 2.25f);
			}
			else
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor, 0.f, DeltaTime, 10.f);
			}
			if (bIsAiming)
			{
				CrosshairAimFactor = FMath::FInterpTo(CrosshairAimFactor, 0.58f, DeltaTime, 10.f);
			}
			else
			{
				CrosshairAimFactor = FMath::FInterpTo(CrosshairAimFactor, 0.f, DeltaTime, 10.f);
			}
			//
			//Shooting比较特殊，他是每次fire，提供一个偏移量，然后迅速回到0，所以在这里，只处理interp到0
			//
			CrosshairShootingFactor = FMath::FInterpTo(CrosshairShootingFactor, 0.f, DeltaTime, 20.f);
			HudPackage.SpreadSpeed =
				.5f +
				CrosshairVelocityFactor +
				CrosshairInAirFactor -
				CrosshairAimFactor +
				CrosshairShootingFactor;

			HUD->SetHUDPackage(HudPackage);
		}
	}
}


bool UCombatComponent::ServerShotGunFire_Validate(const TArray<FVector_NetQuantize>& TraceHitTargets, float FireDelay)
{
	if (EquippedWeapon)
	{
		bool bNearlyEqual = FMath::IsNearlyEqual(EquippedWeapon->FireDelay, FireDelay,0.001f);
		return bNearlyEqual;
	}
	return true;
}

void UCombatComponent::TraceUnderCrosshairs(FHitResult& TraceHitResult, FVector& TraceHitTarget)
{
	FVector2D ViewPortSize;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewPortSize);
	}
	FVector2D CrosshairLocation(ViewPortSize.X / 2.f, ViewPortSize.Y / 2.f);
	FVector CrosshairWorldLocation;
	FVector CrosshairWorldDirection;
	bool bScreenToWorld = UGameplayStatics::DeprojectScreenToWorld(
		UGameplayStatics::GetPlayerController(this, 0),
		CrosshairLocation,
		CrosshairWorldLocation,
		CrosshairWorldDirection
	);
	if (bScreenToWorld)
	{
		FVector Start = CrosshairWorldLocation;

		//
		//为了避免扫描到自己或者很近的actor，我们把StartPoint前置一段距离
		//
			if (Character)
			{
				const float DistanceToCharacter =
					(Character->GetActorLocation() - Start).Size();

				Start += CrosshairWorldDirection * (DistanceToCharacter + 100.f);
		}

		const FVector End = Start + CrosshairWorldDirection * TRACE_LENGTH;

		FCollisionQueryParams Params;
		Params.AddIgnoredActor(Character);
		if (EquippedWeapon) Params.AddIgnoredActor(EquippedWeapon);

			UWorld* World = GetWorld();
			if (World == nullptr) return;

			World->LineTraceSingleByChannel(
				TraceHitResult,
				Start,
			End,
			ECC_Visibility,
			Params
		);
		TraceHitTarget = ResolveTraceHitTarget(TraceHitResult, End);
		HudPackage.CrosshairColor = IsCrosshairTargetActor(TraceHitResult.GetActor())
			                            ? FLinearColor::Red
			                            : FLinearColor::White;
	}
}

FVector UCombatComponent::ResolveTraceHitTarget(const FHitResult& TraceHitResult, const FVector& TraceEnd)
{
	return TraceHitResult.bBlockingHit ? TraceHitResult.ImpactPoint : TraceEnd;
}

bool UCombatComponent::IsCrosshairTargetActor(AActor* HitActor)
{
	if (HitActor == nullptr)
	{
		return false;
	}
	if (HitActor->Implements<UInteractWithCrossHairInterface>())
	{
		return true;
	}
	AWeapon* HitWeapon = Cast<AWeapon>(HitActor);
	AActor* WeaponOwner = HitWeapon ? HitWeapon->GetOwner() : nullptr;
	return WeaponOwner && WeaponOwner->Implements<UInteractWithCrossHairInterface>();
}

void UCombatComponent::UpdateCharacterMovementSpeed()
{
	if (Character == nullptr || Character->GetCharacterMovement() == nullptr) return;

	if (UBuffComponent* Buff = Character->GetBuff())
	{
		const float TargetBaseSpeed = bIsAiming ? AimingSpeed : Buff->GetCurrentBaseSpeed();
		const float TargetCrouchSpeed = Buff->GetCurrentCrouchSpeed();
		Buff->ApplySpeed(TargetBaseSpeed, TargetCrouchSpeed);
		return;
	}

	Character->GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimingSpeed : BaseWalkSpeed;
}

void UCombatComponent::OnRep_Aiming()
{
	if (Character && Character->IsLocallyControlled())
	{
		bIsAiming = bAimingPressed;
	}
	UpdateCharacterMovementSpeed();
}


void UCombatComponent::SetAiming(bool bAiming)
{
	//这样Client能先看到自己蹲下，在传达信息之前，比较流畅
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	bIsAiming = bAiming;
	if (!Character->HasAuthority())
	{
		ServerSetAiming(bAiming);
	}

	UpdateCharacterMovementSpeed();

	if (Character->IsLocallyControlled())
	{
		const bool bShouldRocketCharge = bAiming && ShouldUseRocketChargeFire();

		if (bShouldRocketCharge)
		{
			BeginRocketCharge();
		}
		else if (bIsRocketCharging)
		{
			CancelRocketCharge();
		}

		if (EquippedWeapon->GetWeaponType() == EWeaponType::EWT_SniperRifle)
		{
			Character->ShowSniperScopeWidget(bAiming);
		}
	}
	if (Character->IsLocallyControlled())
	{
		bAimingPressed = bAiming;
	}
}



void UCombatComponent::ServerSetAiming_Implementation(bool bAiming)
{
	bIsAiming = bAiming;
	if (Character)
	{
		UpdateCharacterMovementSpeed();
	}
}


void UCombatComponent::StartTimer()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	Character->GetWorldTimerManager().SetTimer(
		FireTimer,
		this,
		&UCombatComponent::FinishTimer,
		EquippedWeapon->FireDelay
	);
}

void UCombatComponent::FinishTimer()
{
	bCanFire = true;
	if (bFireButtonPressed && EquippedWeapon && EquippedWeapon->bAutomaticWeapon)
	{
		Fire();
	}
	//
	//Fire Start Finish 三循环
	//
}

bool UCombatComponent::CanFire() const
{
	if (EquippedWeapon == nullptr) return false;
	if (bLocallyReload) return false;
	return !EquippedWeapon->IsEmpty() && bCanFire && CombatState == ECombatState::ECS_Unoccupied;
}


void UCombatComponent::InitializeCarriedAmmo()
{
	CarriedAmmoMap.Emplace(EWeaponType::EWT_AssaultRifle, StartingARAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_RocketLaucher, StartingRocketAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_Pistol, StartingPistolAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_SubmachineGun, StartingSubmachineAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_ShotGun, StartingShotGunAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_SniperRifle, StartingSniperRifleAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_GrenadeLaucher, StartingGrenadeLauncherAmmo);
}

void UCombatComponent::Fire()
{
	if (CanFire())
	{
		bCanFire = false;
		if (EquippedWeapon)
		{
			
			//
			//Crosshair
			//
			//
			//后期可以根据武器不同，把这个值变成参量
			//
			CrosshairShootingFactor = 1.4f;
			switch (EquippedWeapon->FireType)
			{
			case EFireType::EFT_Projectile:
				{
					ProjectileFire();
					break;
				}
			case EFireType::EFT_HitScan:
				{
					HitScanFire();
					break;
				}
			case EFireType::EFT_ShotGun:
				{
					ShotGunFire();
					break;
				}
			}
		}
		StartTimer();
	}
}
void UCombatComponent::ProjectileFire()
{
	if (EquippedWeapon && Character)
	{
		HitTarget =  EquippedWeapon->bUseScatter ? EquippedWeapon->TraceEndWithScatter(HitTarget) : HitTarget;
		if (!Character->HasAuthority()) LocalFire(HitTarget);
		ServerFire(HitTarget,EquippedWeapon->FireDelay);
	}
}

void UCombatComponent::HitScanFire()
{
	if (EquippedWeapon && Character)
	{
		HitTarget =  EquippedWeapon->bUseScatter ? EquippedWeapon->TraceEndWithScatter(HitTarget) : HitTarget;
		if (!Character->HasAuthority()) LocalFire(HitTarget);
		ServerFire(HitTarget,EquippedWeapon->FireDelay);
	}
}

void UCombatComponent::ShotGunFire()
{
	AShotGun* ShotGun = Cast<AShotGun>(EquippedWeapon);
	if (ShotGun && Character )
	{
		TArray<FVector_NetQuantize> HitTargets;
		ShotGun->ShotGunTraceEndWithScatter(HitTarget,HitTargets);
		if (!Character->HasAuthority()) ShotGunLocalFire(HitTargets);
		ServerShotGunFire(HitTargets,EquippedWeapon->FireDelay);
	}
}

void UCombatComponent::FireButtonPressed(bool Pressed)
{
	bFireButtonPressed = Pressed;
	if (bFireButtonPressed)
	{
		if (bIsRocketCharging)
		{
			FinishRocketCharge();
			return;
		}
		Fire();
	}
	//
	//client
	//
}

void UCombatComponent::ServerFire_Implementation(const FVector_NetQuantize& TraceHitTarget,float FireDelay)
{
	MulticastFire(TraceHitTarget);
	//
	//server
	//
}

void UCombatComponent::MulticastFire_Implementation(const FVector_NetQuantize& TraceHitTarget)
{
	if (Character && Character->IsLocallyControlled() && !Character->HasAuthority()) return;
	LocalFire(TraceHitTarget);
}

void UCombatComponent::ServerShotGunFire_Implementation(const TArray<FVector_NetQuantize>& TraceHitTargets,float FireDelay)
{
	MulticastShotGunFire(TraceHitTargets);
}

void UCombatComponent::MulticastShotGunFire_Implementation(const TArray<FVector_NetQuantize>& TraceHitTargets)
{
	if (Character && Character->IsLocallyControlled() && !Character->HasAuthority()) return;
	ShotGunLocalFire(TraceHitTargets);
}
void UCombatComponent::LocalFire(const FVector_NetQuantize& TraceHitTarget)
{
	if (EquippedWeapon == nullptr) return;
	if (Character && CombatState == ECombatState::ECS_Unoccupied)
	{
		Character->PlayFireMontage(bIsAiming);
		EquippedWeapon->Fire(TraceHitTarget);
	}
	//
	//Other client
	//
}

void UCombatComponent::ShotGunLocalFire(const TArray<FVector_NetQuantize>& TraceHitTargets)
{
	if (EquippedWeapon == nullptr || Character == nullptr) return;
	AShotGun* ShotGun = Cast<AShotGun>(EquippedWeapon);
	if (ShotGun == nullptr) return;
	if (CombatState == ECombatState::ECS_Unoccupied)
	{
		Character->PlayFireMontage(bIsAiming);
		ShotGun->ShotGunFire(TraceHitTargets);
		CombatState = ECombatState::ECS_Unoccupied;
	}
}

bool UCombatComponent::ServerFire_Validate(const FVector_NetQuantize& TraceHitTarget, float FireDelay)
{
	if (EquippedWeapon)
	{
		bool bNearlyEqual = FMath::IsNearlyEqual(EquippedWeapon->FireDelay, FireDelay,0.001f);
		return bNearlyEqual;
	}
	return true;
}

void UCombatComponent::BroadcastImpactEffect(UNiagaraSystem* ImpactEffect, USoundCue* ImpactSound,
                                             const FVector& ImpactLocation, const FVector& ImpactNormal)
{
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority()) return;
	if (ImpactEffect == nullptr && ImpactSound == nullptr) return;

	// The server is the only machine that confirms hits; clients only receive the confirmed impact point.
	MulticastImpactEffect(ImpactEffect, ImpactSound, ImpactLocation, ImpactNormal.GetSafeNormal());
}

void UCombatComponent::PickUpAmmo(EWeaponType WeaponType, int32 AmmoAmount)
{
	if (AmmoAmount <= 0) return;

	int32* CarriedAmmoForWeapon = CarriedAmmoMap.Find(WeaponType);
	if (CarriedAmmoForWeapon == nullptr) return;

	*CarriedAmmoForWeapon += AmmoAmount;

	if (EquippedWeapon && EquippedWeapon->GetWeaponType() == WeaponType)
	{
		CarriedAmmo = *CarriedAmmoForWeapon;

		Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character ? Character->GetController() : nullptr) : Controller;
		if (Controller)
		{
			Controller->SetCarriedAmmo(CarriedAmmo);
		}

		if (EquippedWeapon->IsEmpty())
		{
			Reload();
		}
	}
}

void UCombatComponent::SetGrenadeAmount(int32 Amount)
{
	GrenadeAmount = FMath::Clamp(Amount, 0, MaxGrenadeAmount);
	if (Character && Character->HasAuthority())
	{
		if (ABlasterPlayerState* BlasterPlayerState = Character->GetPlayerState<ABlasterPlayerState>())
		{
			BlasterPlayerState->SetGrenadeAmount(GrenadeAmount);
		}
	}
	UpdateGrenadeAmount();
}

void UCombatComponent::MulticastImpactEffect_Implementation(UNiagaraSystem* ImpactEffect, USoundCue* ImpactSound,
                                                            const FVector_NetQuantize& ImpactLocation,
                                                            const FVector_NetQuantizeNormal& ImpactNormal)
{
	UWorld* World = GetWorld();
	if (World == nullptr) return;

	const FRotator ImpactRotation = ImpactNormal.IsNearlyZero()
		                                ? FRotator::ZeroRotator
		                                : ImpactNormal.Rotation();

	if (ImpactSound)
	{
		UGameplayStatics::PlaySoundAtLocation(World, ImpactSound, ImpactLocation);
	}

	if (ImpactEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World,
			ImpactEffect,
			ImpactLocation,
			ImpactRotation,
			FVector(1.f),
			true,
			true,
			ENCPoolMethod::AutoRelease,
			true
		);
	}
}


void UCombatComponent::EquipWeapon(AWeapon* WeaponToEquipped)
{
	if (Character == nullptr || WeaponToEquipped == nullptr || CombatState != ECombatState::ECS_Unoccupied)
	{
		return;
	}

	if (EquippedWeapon == nullptr)
	{
		if (!IsPistolWeapon(WeaponToEquipped))
		{
			PrimaryWeapon = WeaponToEquipped;
		}
		EquippedWeapon = WeaponToEquipped;
		EquipWeaponInHand(EquippedWeapon);
		UpdateCarriedAmmoForEquippedWeapon();
		UpdateWeaponHUD();
	}
	else
	{
		if (SecondaryWeapon == nullptr && IsPistolWeapon(EquippedWeapon))
		{
			EquipSecondaryWeapon(EquippedWeapon);
		}
		EquipPrimaryWeapon(WeaponToEquipped);
	}
	//当我们拿到枪之后希望能够跟着Controller转换视角
	//当前这个只在Server端享受,其他Client也能跟着看见但是当前的看不见，所以需要单独对这个做调整？谁能指向当前这个Client呢？Equip确实是当前CLient独有的，所以加一个复制Callback
	Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = true;
}

void UCombatComponent::SwapWeapon()
{
	if (Character == nullptr || !Character->HasAuthority()) return;
	if (EquippedWeapon == nullptr || PrimaryWeapon == nullptr || SecondaryWeapon == nullptr ||
		CombatState != ECombatState::ECS_Unoccupied)
	{
		return;
	}
	Character->PlaySwapMontage();
		Character->bFinishSwapping = false;
	bSwapAttachCompleted = false;
	CombatState = ECombatState::ECS_Swaping;

	AWeapon* WeaponOnBack = EquippedWeapon == PrimaryWeapon ? SecondaryWeapon : PrimaryWeapon;
	if (WeaponOnBack)
	{
		WeaponOnBack->EnableCustomDepth(false);
	}
}

bool UCombatComponent::ShouldSwap()
{
	return PrimaryWeapon != nullptr &&
		SecondaryWeapon != nullptr &&
		CombatState == ECombatState::ECS_Unoccupied;
}

void UCombatComponent::EquipPrimaryWeapon(AWeapon* EquipWeapon)
{
	if (EquipWeapon== nullptr) return; 
	if (PrimaryWeapon && PrimaryWeapon != EquipWeapon)
	{
		PrimaryWeapon->Dropped();
	}
	PrimaryWeapon = EquipWeapon;
	EquippedWeapon = PrimaryWeapon;
	EquipWeaponInHand(EquippedWeapon);

	if (EquippedWeapon->PickUpSound)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			EquippedWeapon->PickUpSound,
			Character->GetActorLocation());
	}

	if (SecondaryWeapon)
	{
		StowWeapon(SecondaryWeapon);
	}
	UpdateCarriedAmmoForEquippedWeapon();
	UpdateWeaponHUD();
}

void UCombatComponent::EquipSecondaryWeapon(AWeapon* EquipWeapon)
{
	if(EquipWeapon == nullptr) return;
	SecondaryWeapon = EquipWeapon;
	StowWeapon(SecondaryWeapon);
}

void UCombatComponent::EquipWeaponInHand(AWeapon* WeaponToEquip)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || WeaponToEquip == nullptr) return;

	WeaponToEquip->SetWeaponState(EWeaponState::EWS_Equipped);
	FName AttachSocketName =
		WeaponToEquip->GetWeaponType() == EWeaponType::EWT_RocketLaucher
			? FName("RPGRHandSocket")
			: FName("RHandSocket");

	WeaponToEquip->AttachToComponent(
		Character->GetMesh(),
		FAttachmentTransformRules::SnapToTargetNotIncludingScale,
		AttachSocketName
	);
	WeaponToEquip->SetOwner(Character);
}

void UCombatComponent::StowWeapon(AWeapon* WeaponToStow)
{
	if (Character == nullptr || WeaponToStow == nullptr) return;

	AttachWeaponToBackPack(WeaponToStow);
	WeaponToStow->SetWeaponState(EWeaponState::EWS_EquipedSecondary);
	WeaponToStow->SetOwner(Character);
}

void UCombatComponent::UpdateCarriedAmmoForEquippedWeapon()
{
	CarriedAmmo = 0;
	if (EquippedWeapon == nullptr) return;

	if (const int32* CarriedAmmoForWeapon = CarriedAmmoMap.Find(EquippedWeapon->GetWeaponType()))
	{
		CarriedAmmo = *CarriedAmmoForWeapon;
	}
}

void UCombatComponent::UpdateWeaponHUD()
{
	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character ? Character->GetController() : nullptr) : Controller;
	if (Controller)
	{
		Controller->SetCarriedAmmo(CarriedAmmo);
		Controller->SetWeaponAmmoVisible(EquippedWeapon != nullptr);
	}
	if (EquippedWeapon)
	{
		EquippedWeapon->SetHUDAmmo();
	}
}

bool UCombatComponent::IsPistolWeapon(AWeapon* Weapon) const
{
	return Weapon && Weapon->GetWeaponType() == EWeaponType::EWT_Pistol;
}

void UCombatComponent::OnRep_EquippedWeapon()
{
	if (EquippedWeapon && Character)
	{
		//因为client不一定会设置state
		EquipWeaponInHand(EquippedWeapon);
		if (PrimaryWeapon && PrimaryWeapon != EquippedWeapon)
		{
			StowWeapon(PrimaryWeapon);
		}
		if (SecondaryWeapon && SecondaryWeapon != EquippedWeapon)
		{
			StowWeapon(SecondaryWeapon);
		}
		//Character即当前Client，这样处理可以，EquipWeapon这里无论如何都是在server上，所以那个Character不是当前client
		Character->GetCharacterMovement()->bOrientRotationToMovement = false;
		Character->bUseControllerRotationYaw = true;
		if (EquippedWeapon->PickUpSound)
		{
				UGameplayStatics::PlaySoundAtLocation(
					this,
					EquippedWeapon->PickUpSound,
					Character->GetActorLocation());
		}
		EquippedWeapon->SetHUDAmmo();
	}
}

void UCombatComponent::OnRep_PrimaryWeapon()
{
	if (Character && PrimaryWeapon)
	{
		if (PrimaryWeapon == EquippedWeapon)
		{
			EquipWeaponInHand(PrimaryWeapon);
			PrimaryWeapon->SetHUDAmmo();
		}
		else
		{
			StowWeapon(PrimaryWeapon);
		}
	}
}

void UCombatComponent::OnRep_SecondaryWeapon()
{
	if (Character && SecondaryWeapon)
	{
		if (SecondaryWeapon == EquippedWeapon)
		{
			EquipWeaponInHand(SecondaryWeapon);
			SecondaryWeapon->SetHUDAmmo();
		}
		else
		{
			StowWeapon(SecondaryWeapon);
		}
	}
}

void UCombatComponent::OnRep_CarriedAmmo()
{
	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetCarriedAmmo(CarriedAmmo);
	}
	bool bShouldJumpToEnd =
		ECombatState::ECS_Reloading == CombatState &&
		EquippedWeapon != nullptr &&
		EquippedWeapon->GetWeaponType() == EWeaponType::EWT_ShotGun &&
		CarriedAmmo == 0;
	if (bShouldJumpToEnd)
	{
		JumpToShotGunEnd();
	}
}

void UCombatComponent::ThrowingGrenadeFinish()
{
	if (CombatState != ECombatState::ECS_Throwing) return;

	CombatState = ECombatState::ECS_Unoccupied;
	AttachWeaponToRHand();
	if (Character && !Character->HasAuthority())
	{
		ServerThrowGrenadeFinish();
	}

	if (bFireButtonPressed)
	{
		Fire();
	}
}

void UCombatComponent::LaunchGrenade()
{
	ShowGrenade(false);

	if (Character == nullptr) return;
	if (!Character->IsLocallyControlled()) return;

	if (Character->HasAuthority())
	{
		SpawnGrenade(HitTarget);
	}
	else
	{
		ServerLaunchGrenade(HitTarget);
	}
}

void UCombatComponent::ServerLaunchGrenade_Implementation(const FVector_NetQuantize& TraceHitTarget)
{
	SpawnGrenade(TraceHitTarget);
}

void UCombatComponent::ServerThrowGrenadeFinish_Implementation()
{
	if (CombatState != ECombatState::ECS_Throwing) return;

	CombatState = ECombatState::ECS_Unoccupied;
	AttachWeaponToRHand();
}

void UCombatComponent::SpawnGrenade(const FVector_NetQuantize& TraceHitTarget)
{
	if (Character == nullptr || !Character->HasAuthority()) return;
	if (Character->GetAttachedGrenade() == nullptr) return;
	if (GrenadeClass == nullptr) return;

	UWorld* World = GetWorld();
	if (World == nullptr) return;

	const FVector StartLocation = Character->GetAttachedGrenade()->GetComponentLocation();
	FVector ToTarget = FVector(TraceHitTarget) - StartLocation;

	if (ToTarget.IsNearlyZero())
	{
		ToTarget = Character->GetActorForwardVector();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Character;
	SpawnParams.Instigator = Character;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	World->SpawnActor<AMyProjectile>(
		GrenadeClass,
		StartLocation,
		ToTarget.Rotation(),
		SpawnParams
	);
}

void UCombatComponent::ShotGunShellReload()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	if (!Character->HasAuthority()) return;
	if (CombatState != ECombatState::ECS_Reloading) return;
	if (EquippedWeapon->GetWeaponType() != EWeaponType::EWT_ShotGun) return;
	UpdateShotGunAmmoValues();
}

void UCombatComponent::Reload()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	//不能一直重读播放动画
	//而且结束了要变回Unocuppy
	if (CarriedAmmo > 0 && CombatState == ECombatState::ECS_Unoccupied && EquippedWeapon->GetAmmo() < EquippedWeapon->
		GetMagCapacity() && !bLocallyReload)
	{
		ServerReload();
		if (Character->IsLocallyControlled())
		{
			bLocallyReload = true;
			HandleReload();
		}
		//到server
	}
}
void UCombatComponent::ServerReload_Implementation()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	if (CarriedAmmo <= 0 ||
		CombatState != ECombatState::ECS_Unoccupied ||
		EquippedWeapon->GetAmmo() >= EquippedWeapon->GetMagCapacity())
	{
		return;
	}
	CombatState = ECombatState::ECS_Reloading;
	if (!Character->IsLocallyControlled()) HandleReload();
	//State Change ,To OnRep
}

void UCombatComponent::HandleReload()
{
	if (Character == nullptr) return;
	Character->PlayReloadMontage();
}

void UCombatComponent::FinishReload()
{
	if (Character == nullptr) return;
	bLocallyReload = false;
	if (Character->HasAuthority())
	{
		CombatState = ECombatState::ECS_Unoccupied;
		if (EquippedWeapon &&
			EquippedWeapon->GetWeaponType() != EWeaponType::EWT_ShotGun)
		{
			UpdateAmmoValues();
		}
	}
	if (bFireButtonPressed)
	{
		Fire();
	}
}

void UCombatComponent::FinishSwaping()
{
	if (Character && !Character->HasAuthority() && Character->IsLocallyControlled())
	{
		ServerFinishSwaping();
	}
	if (Character)
	{
		CombatState = ECombatState::ECS_Unoccupied;
		Character->bFinishSwapping = true;
	}

	AWeapon* WeaponOnBack = EquippedWeapon == PrimaryWeapon ? SecondaryWeapon : PrimaryWeapon;
	if (WeaponOnBack)
	{
		WeaponOnBack->EnableCustomDepth(true);
	}
}

void UCombatComponent::ServerFinishSwaping_Implementation()
{
	FinishSwaping();
}

void UCombatComponent::FinishSwapAttachWeapon()
{
	if (EquippedWeapon == nullptr || PrimaryWeapon == nullptr || SecondaryWeapon == nullptr) return;
	if (Character && Character->HasAuthority() && bSwapAttachCompleted) return;

	AWeapon* NewEquippedWeapon = EquippedWeapon == PrimaryWeapon ? SecondaryWeapon : PrimaryWeapon;
	if (NewEquippedWeapon && NewEquippedWeapon->PickUpSound && Character)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			NewEquippedWeapon->PickUpSound,
			Character->GetActorLocation()
			);
	}

	if (Character == nullptr || NewEquippedWeapon == nullptr) return;
	if (!Character->HasAuthority())
	{
		if (Character->IsLocallyControlled())
		{
			ServerFinishSwapAttachWeapon();
		}
		return;
	}

	bSwapAttachCompleted = true;
	AWeapon* OldEquippedWeapon = EquippedWeapon;
	StowWeapon(OldEquippedWeapon);
	EquippedWeapon = NewEquippedWeapon;
	EquipWeaponInHand(EquippedWeapon);
	UpdateCarriedAmmoForEquippedWeapon();
	UpdateWeaponHUD();
}

void UCombatComponent::ServerFinishSwapAttachWeapon_Implementation()
{
	FinishSwapAttachWeapon();
}

int32 UCombatComponent::AmountToReload()
{
	if (EquippedWeapon == nullptr) return 0;
	int32 RoomInMag = EquippedWeapon->GetMagCapacity() - EquippedWeapon->GetAmmo();
	if (CarriedAmmoMap.Contains(EquippedWeapon->GetWeaponType()))
	{
		//如果调用错误会出负数，所以用一下fangfa
		int32 AmountCarried = CarriedAmmoMap[EquippedWeapon->GetWeaponType()];
		int32 Least = FMath::Min(AmountCarried, RoomInMag);
		return FMath::Clamp(RoomInMag, 0, Least);
	}
	return 0;
}

void UCombatComponent::UpdateAmmoValues()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;
	int32 ReloadAmount = AmountToReload();
	//更新Character弹药
	if (CarriedAmmoMap.Contains(EquippedWeapon->GetWeaponType()))
	{
		CarriedAmmoMap[EquippedWeapon->GetWeaponType()] -= ReloadAmount;
		CarriedAmmo = CarriedAmmoMap[EquippedWeapon->GetWeaponType()];
	}
	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetCarriedAmmo(CarriedAmmo);
	}
	//更新武器Ammo
	EquippedWeapon->AddAmmo(ReloadAmount);
}

void UCombatComponent::UpdateShotGunAmmoValues()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return;

	const EWeaponType WeaponType = EquippedWeapon->GetWeaponType();
	int32* CarriedAmmoForWeapon = CarriedAmmoMap.Find(WeaponType);
	if (CarriedAmmoForWeapon == nullptr) return;

	if (*CarriedAmmoForWeapon <= 0 || EquippedWeapon->IsFull())
	{
		JumpToShotGunEnd();
		return;
	}

	*CarriedAmmoForWeapon = FMath::Max(*CarriedAmmoForWeapon - 1, 0);
	CarriedAmmo = *CarriedAmmoForWeapon;

	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetCarriedAmmo(CarriedAmmo);
	}

	EquippedWeapon->AddAmmo(1);

	if (EquippedWeapon->IsFull() || CarriedAmmo <= 0)
	{
		JumpToShotGunEnd();
	}
}


void UCombatComponent::ThrowGrenade()
{
	if (GrenadeAmount == 0) return;
	if (Character == nullptr || EquippedWeapon == nullptr || GrenadeClass == nullptr) return;
	if (CombatState != ECombatState::ECS_Unoccupied) return;

	CombatState = ECombatState::ECS_Throwing;

	Character->PlayThrowMontage();
	AttachWeaponToLHand(EquippedWeapon);
	ShowGrenade(true);

	if (!Character->HasAuthority())
	{
		ServerThrowGrenade();
	}else
	{
		SetGrenadeAmount(GrenadeAmount - 1);
	}
	
}
void UCombatComponent::ServerThrowGrenade_Implementation()
{
	if (GrenadeAmount == 0) return;
	if (Character == nullptr || EquippedWeapon == nullptr || GrenadeClass == nullptr) return;
	if (CombatState != ECombatState::ECS_Unoccupied) return;

	CombatState = ECombatState::ECS_Throwing;

	Character->PlayThrowMontage();
	AttachWeaponToLHand(EquippedWeapon);
	ShowGrenade(true);
	SetGrenadeAmount(GrenadeAmount - 1);
}
void UCombatComponent::UpdateGrenadeAmount()
{
	Controller = Controller == nullptr && Character ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDGrenade(GrenadeAmount);
	}
}
void UCombatComponent::AttachWeaponToLHand(AActor* ActorToAttach)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr) return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(FName("LHandSocket"));
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach, Character->GetMesh());
	}
}

void UCombatComponent::AttachWeaponToRHand()
{
	if (EquippedWeapon && Character && Character->GetMesh())
	{
		FName AttachSocketName =
			EquippedWeapon->GetWeaponType() == EWeaponType::EWT_RocketLaucher
				? FName("RPGRHandSocket")
				: FName("RHandSocket");

		EquippedWeapon->AttachToComponent(
			Character->GetMesh(),
			FAttachmentTransformRules::SnapToTargetNotIncludingScale,
			AttachSocketName
		);
	}
}

void UCombatComponent::AttachWeaponToBackPack(AActor* ActorToAttach)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr) return;
	const USkeletalMeshSocket* BackPackSocket = Character->GetMesh()->GetSocketByName(FName("BackPackSocket"));
	if (BackPackSocket)
	{
		BackPackSocket->AttachActor(ActorToAttach, Character->GetMesh());
	}
}


void UCombatComponent::ShowGrenade(bool bShouldShow)
{
	if (Character && Character->GetAttachedGrenade())
	{
		Character->GetAttachedGrenade()->SetVisibility(bShouldShow);
	}
}

void UCombatComponent::JumpToShotGunEnd()
{
	if (Character == nullptr || Character->GetMesh() == nullptr) return;

	UAnimInstance* AnimInstance = Character->GetMesh()->GetAnimInstance();
	UAnimMontage* ReloadMontage = Character->GetReloadMontage();
	if (AnimInstance && ReloadMontage)
	{
		AnimInstance->Montage_JumpToSection(FName("ShotGunEnd"), ReloadMontage);
	}
}

void UCombatComponent::OnRep_Grenade() 
{
	UpdateGrenadeAmount();
}

void UCombatComponent::OnRep_CombatState()
{
	switch (CombatState)
	{
	case ECombatState::ECS_Reloading:
		{
			if (Character && !Character->IsLocallyControlled())
			{
				HandleReload();
			}
			break;
		}
	case ECombatState::ECS_Unoccupied:
		{
			if (bFireButtonPressed)
			{
				Fire();
			}
			break;
		}
	case ECombatState::ECS_Throwing:
		{
			if (Character && !Character->IsLocallyControlled())
			{
				Character->PlayThrowMontage();
				AttachWeaponToLHand(EquippedWeapon);
				ShowGrenade(true);
			}
			break;
		}
	case ECombatState::ECS_Swaping:
		{
			if (Character && !Character->IsLocallyControlled())
			{
				Character->bFinishSwapping = false;
				Character->PlaySwapMontage();
			}
			break;	
		}
	}
}

bool UCombatComponent::ShouldUseRocketChargeFire()
{
	if (Character == nullptr || EquippedWeapon == nullptr) return false;
	if (!Character->IsLocallyControlled()) return false;

	return EquippedWeapon->ChargeSound != nullptr &&
		!EquippedWeapon->IsEmpty() && EquippedWeapon->bUsedRocketChargeFire;
}

void UCombatComponent::BeginRocketCharge()
{
	if (!ShouldUseRocketChargeFire()) return;
	if (bIsRocketCharging) return;

	bIsRocketCharging = true;
	RocketChargeStartedTime = GetWorld()->GetTimeSeconds();
	RocketChargeStartFOV = CurrentFOV;

	if (EquippedWeapon->ChargeSound)
	{
		RocketChargeAudioComponent = UGameplayStatics::SpawnSound2D(
			this,
			EquippedWeapon->ChargeSound);
	}
	Character->GetWorldTimerManager().SetTimer(
		RocketChargeTimer,
		this,
		&UCombatComponent::FinishRocketCharge,
		EquippedWeapon->ChargeDuration,
		false
	);
}

void UCombatComponent::CancelRocketCharge()
{
	if (!bIsRocketCharging) return;
	bIsRocketCharging = false;

	if (Character)
	{
		Character->GetWorldTimerManager().ClearTimer(RocketChargeTimer);
	}

	if (RocketChargeAudioComponent)
	{
		RocketChargeAudioComponent->Stop();
		RocketChargeAudioComponent = nullptr;
	}
}

void UCombatComponent::FinishRocketCharge()
{
	if (!bIsRocketCharging) return;
	bIsRocketCharging = false;
	if (Character)
	{
		Character->GetWorldTimerManager().ClearTimer(RocketChargeTimer);
	}
	if (RocketChargeAudioComponent)
	{
		RocketChargeAudioComponent->Stop();
		RocketChargeAudioComponent = nullptr;
	}

	if (EquippedWeapon && EquippedWeapon->ReleaseSound)
	{
		UGameplayStatics::PlaySound2D(this, EquippedWeapon->ReleaseSound);
	}
	Fire();
}
