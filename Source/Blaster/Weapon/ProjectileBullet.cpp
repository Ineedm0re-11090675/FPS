#include "ProjectileBullet.h"

#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/BlasterComponent/LagCompensationComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "GameFramework/Character.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AProjectileBullet::AProjectileBullet()
{
	// Bullet impacts are server-confirmed and broadcast through CombatComponent, not actor destruction.
	bPlayImpactEffectsOnDestroyed = false;

	MovementComponent= CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("MovementComponent"));
	//from codex
	MovementComponent->SetUpdatedComponent(CollisionBox);
	//这样可以影响速度方向来控制子弹的方向，加重力
	MovementComponent->bRotationFollowsVelocity = true;
	MovementComponent->bShouldBounce = false;
	MovementComponent->Bounciness = 0.f;
	MovementComponent->ProjectileGravityScale = 0.f;
	MovementComponent->SetIsReplicated(true);

	MovementComponent->InitialSpeed =InitSpeed;
	MovementComponent->MaxSpeed = InitSpeed;
}

void AProjectileBullet::BeginPlay()
{
	Super::BeginPlay();
}

void AProjectileBullet::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                              FVector NormalImpulse, const FHitResult& Hit)
{
	UE_LOG(LogTemp, Verbose, TEXT("[ProjectileBullet::OnHit] Auth=%d bSSR=%d OtherActor=%s OtherComp=%s Owner=%s Instigator=%s"),
		HasAuthority(),
		bUseServerRewind,
		*GetNameSafe(OtherActor),
		*GetNameSafe(OtherComp),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(GetInstigator())
	);

	if (OtherActor == nullptr || OtherActor == GetOwner() || OtherActor == GetInstigator())
	{
		return;
	}

	AMyBlasterCharacter* OwnerCharacter=Cast<AMyBlasterCharacter>(GetOwner());
	ABlasterPlayerController* OwnerController = OwnerCharacter ? Cast<ABlasterPlayerController>(OwnerCharacter->GetController()) : nullptr;
	AMyBlasterCharacter* HitCharacter = Cast<AMyBlasterCharacter>(OtherActor);

	if (OwnerCharacter && OwnerController)
	{
		bool bCauseAuthDamage = !bUseServerRewind || OwnerCharacter->IsLocallyControlled();
		if (OwnerCharacter->HasAuthority() && bCauseAuthDamage)
		{
			const float DamageToCause = Hit.BoneName.ToString() == FString("head")
				? Damage*HeadShotDamageRatio : Damage;
			
			UGameplayStatics::ApplyDamage(
				OtherActor,
				DamageToCause,
				OwnerController,
				this,
				UDamageType::StaticClass()
			);
			Super::OnHit(HitComp, OtherActor, OtherComp, NormalImpulse, Hit);
			return; 
		}
		if (bUseServerRewind && OwnerCharacter->IsLocallyControlled() && HitCharacter && OwnerCharacter->GetLagCompensation())
		{
			OwnerCharacter->GetLagCompensation()->ProjectileServerScoreRequest(
				HitCharacter,
				StartLocation,
				InitVelocity,
				OwnerController->GetServerTime() - OwnerController->SingleTripTime
			);
		}
		if (UCombatComponent* Combat = OwnerCharacter->GetCombat())
		{
			if (Hit.bBlockingHit)
			{
				Combat->BroadcastImpactEffect(
					ImpactNia,
					ImpactSound,
					Hit.ImpactPoint,
					Hit.ImpactNormal
				);
			}
		}
	}
	//hui destroy
	Super::OnHit(HitComp, OtherActor, OtherComp, NormalImpulse, Hit);
}

#if WITH_EDITOR
void AProjectileBullet::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	FName PropertyName= PropertyChangedEvent.Property != nullptr ?  PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AProjectileBullet, InitSpeed))
	{
		if (MovementComponent)
		{
			MovementComponent->InitialSpeed =InitSpeed;
			MovementComponent->MaxSpeed = InitSpeed;
		}
	}
}
#endif
