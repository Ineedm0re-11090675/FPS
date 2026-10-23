#include "HitScanWeapon.h"
#include "NiagaraFunctionLibrary.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"
#include "NiagaraComponent.h"
#include "Blaster/BlasterComponent/LagCompensationComponent.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"

void AHitScanWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget);

	const USkeletalMeshSocket* MuzzleFlashSocket = GetWeaponMesh()->GetSocketByName("FireFlashSocket");
	if (MuzzleFlashSocket == nullptr) return;

	const FTransform SocketTransform = MuzzleFlashSocket->GetSocketTransform(GetWeaponMesh());
	const FVector Start = SocketTransform.GetLocation();
	FHitResult HitResult;
	WeaponHitTrace(Start, HitTarget, HitResult);

	UWorld* World = GetWorld();
	AMyBlasterCharacter* FiringCharacter = Cast<AMyBlasterCharacter>(GetOwner());
	if (FiringCharacter == nullptr) return;
	
	AController* FiringController = FiringCharacter->GetController();
	ABlasterPlayerController* BlasterFiringController = Cast<ABlasterPlayerController>(FiringController);
	AMyBlasterCharacter* HitCharacter = Cast<AMyBlasterCharacter>(HitResult.GetActor());

	const bool bCauseAuthDamage =
		HasAuthority() &&
		HitCharacter &&
		FiringController &&
		(!bUseServerRewind || FiringCharacter->IsLocallyControlled());
	if (bCauseAuthDamage)
	{
		const float DamageToCause = HitResult.BoneName.ToString() == FString("head") ? Damage * HeadShotDamageRatio : Damage;
		UGameplayStatics::ApplyDamage(
			HitCharacter,
			DamageToCause,
			FiringController,
			this,
			UDamageType::StaticClass()
		);
	}

	const bool bRequestServerRewind =
		!HasAuthority() &&
		bUseServerRewind &&
		FiringCharacter->IsLocallyControlled() &&
		HitCharacter &&
		BlasterFiringController &&
		FiringCharacter->GetLagCompensation();
	if (bRequestServerRewind)
	{
			FiringCharacter->GetLagCompensation()->ServerScoreRequest(
			HitCharacter,
			Start,
			HitTarget,
			// Client gets server time after one round trip, so subtract half of the trip time for the shot moment.
			BlasterFiringController->GetServerTime() - BlasterFiringController->SingleTripTime,
			this
		);
	}

	if (HitResult.bBlockingHit)
	{
		if (HasAuthority())
		{
			if (UCombatComponent* Combat = FiringCharacter->GetCombat())
			{
				Combat->BroadcastImpactEffect(
					ImpactParticle,
					nullptr,
					HitResult.ImpactPoint,
					HitResult.ImpactNormal
				);
			}
		}
		if (HitSound)
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				HitSound,
				HitResult.ImpactPoint
				);
		}
	}
		
	if (MuzzleFLash)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World,
			MuzzleFLash,
			SocketTransform.GetLocation()
			);
	}
	if (FireSound)
	{
		UGameplayStatics::PlaySoundAtLocation(
			World,
			FireSound,
			SocketTransform.GetLocation()
		);
	}
	
}


void AHitScanWeapon::WeaponHitTrace(const FVector& TraceStart, const FVector& HitTarget, FHitResult& OutHit)
{
	UWorld* World = GetWorld();
	AMyBlasterCharacter* OwnerCharacter = Cast<AMyBlasterCharacter>(GetOwner());
	
	if (World)
	{
		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(OwnerCharacter);
		QueryParams.AddIgnoredActor(this);
		FVector End= TraceStart + (HitTarget - TraceStart) * 1.25f;
		World->LineTraceSingleByChannel(
			OutHit,
			TraceStart,
			End,
			ECollisionChannel::ECC_Visibility,
			QueryParams
		);
		FVector BeamEnd = End;
		if (OutHit.bBlockingHit)
		{
			BeamEnd = OutHit.ImpactPoint;
		}
		if (BeamParticle)
		{
			UNiagaraComponent* Beam = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
				World,
				BeamParticle,
				TraceStart
			);

			if (Beam)
			{
				Beam->SetVariablePosition(FName("User.BeamStart"), TraceStart);
				Beam->SetVariablePosition(FName("User.BeamEnd"), BeamEnd);
			}
		}
	}
}
