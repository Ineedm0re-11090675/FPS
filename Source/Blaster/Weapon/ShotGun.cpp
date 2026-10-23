#include "ShotGun.h"

#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/BlasterComponent/LagCompensationComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"

void AShotGun::ShotGunFire(const TArray<FVector_NetQuantize>& TraceHitTargets)
{
	AWeapon::Fire(FVector());

	AMyBlasterCharacter* FiringCharacter = Cast<AMyBlasterCharacter>(GetOwner());
	if (FiringCharacter == nullptr) return;
	AController* FiringController = FiringCharacter->GetController();
	ABlasterPlayerController* BlasterFiringController = Cast<ABlasterPlayerController>(FiringController);
		
	const USkeletalMeshSocket* MuzzleFlashSocket = GetWeaponMesh()->GetSocketByName("FireFlashSocket");
	if (MuzzleFlashSocket == nullptr) return;

	const FTransform SocketTransform = MuzzleFlashSocket->GetSocketTransform(GetWeaponMesh());
	const FVector Start = SocketTransform.GetLocation();

	TMap<AMyBlasterCharacter*, uint32> HitMap;
	TMap<AMyBlasterCharacter*, uint32> HeadShotMap;
	for (const FVector_NetQuantize& HitTarget : TraceHitTargets)
		{
			FHitResult FireHit;
			WeaponHitTrace(Start, HitTarget, FireHit);
		
			if (!FireHit.bBlockingHit) continue;

		AMyBlasterCharacter* HitCharacter = Cast<AMyBlasterCharacter>(FireHit.GetActor());
		if (HitCharacter)
		{
			bool bHeadShot = FireHit.BoneName.ToString() == FString("head");
			if (bHeadShot)
			{
				if (HeadShotMap.Contains(HitCharacter))
				{
					HeadShotMap[HitCharacter]++;
				}else
				{
					HeadShotMap.Emplace(HitCharacter, 1);
				}
			}else
			{
				if (HitMap.Contains(HitCharacter))
				{
					HitMap[HitCharacter]++;
				}else
				{
					HitMap.Emplace(HitCharacter, 1);
				}
			}
		}
		if (HasAuthority())
		{
			if (UCombatComponent* Combat = FiringCharacter->GetCombat())
			{
				Combat->BroadcastImpactEffect(
					ImpactParticle,
					HitSound,
					FireHit.ImpactPoint,
					FireHit.ImpactNormal
				);
			}
		}
	}

	TArray<AMyBlasterCharacter*> HitCharacters;
	TMap<AMyBlasterCharacter*, uint32> DamageToCause;
	for (const auto& HitPair : HitMap)
	{
		if (HitPair.Key == nullptr) continue;
		DamageToCause.Emplace(HitPair.Key, HitPair.Value * Damage);
		HitCharacters.AddUnique(HitPair.Key);
	}
	for (const auto& HitPair : HeadShotMap)
	{
		if (HitPair.Key == nullptr) continue;
		if (DamageToCause.Contains(HitPair.Key))
		{
			DamageToCause[HitPair.Key]+=HitPair.Value*Damage * HeadShotDamageRatio;
		}else
		{
			DamageToCause.Emplace(HitPair.Key, HitPair.Value*Damage * HeadShotDamageRatio);
		}
		HitCharacters.AddUnique(HitPair.Key);
	}
	for (const auto& HitPair : DamageToCause)
	{
		if (HitPair.Key  && FiringController)
		{
			const bool bCauseAuthDamage =
				HasAuthority() &&
				FiringController &&
				(!bUseServerRewind || FiringCharacter->IsLocallyControlled());
			if (bCauseAuthDamage)
			{
				UGameplayStatics::ApplyDamage(
					HitPair.Key,
					HitPair.Value,
					FiringController,
					this,
					UDamageType::StaticClass()
				);
			}
		}
	}
	const bool bRequestServerRewind =
		!HasAuthority() &&
		bUseServerRewind &&
		FiringCharacter->IsLocallyControlled() &&
		HitCharacters.Num() > 0 &&
		BlasterFiringController &&
		FiringCharacter->GetLagCompensation();
	if (bRequestServerRewind)
	{
		FiringCharacter->GetLagCompensation()->ShotGunServerScoreRequest(
			HitCharacters,
			Start,
			TraceHitTargets,
			BlasterFiringController->GetServerTime() - BlasterFiringController->SingleTripTime,
			this
		);
	}
}

void AShotGun::ShotGunTraceEndWithScatter(const FVector& HitTarget, TArray<FVector_NetQuantize>& HitTargets)
{
	const USkeletalMeshSocket* MuzzleFlashSocket = GetWeaponMesh()->GetSocketByName("FireFlashSocket");
	if (MuzzleFlashSocket == nullptr) return;
	const FTransform SocketTransform = MuzzleFlashSocket->GetSocketTransform(GetWeaponMesh());
	const FVector TraceStart = SocketTransform.GetLocation();
		
	const FVector ToTargetNormalized = (HitTarget - TraceStart).GetSafeNormal();
	const FVector SphereCenter = TraceStart + ToTargetNormalized * DistanceToSphere;
	for (uint32 i=0;i<NumberOfPellets;i++)
	{
		const FVector RandVec = UKismetMathLibrary::RandomUnitVector() * FMath::FRandRange(0.f,SphereRadius);
		const FVector EndLoc= SphereCenter + RandVec;
		FVector ToEndLoc= EndLoc - TraceStart;
		const FVector TraceDirection = ToEndLoc.IsNearlyZero() ? ToTargetNormalized : ToEndLoc.GetSafeNormal();
		HitTargets.Add(TraceStart + TraceDirection * TRACE_LENGTH);
	}
}
