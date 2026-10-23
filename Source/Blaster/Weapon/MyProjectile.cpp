// Fill out your copyright notice in the Description page of Project Settings.


#include "MyProjectile.h"
#include "Components/BoxComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "../Blaster.h"
#include "Sound/SoundCue.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
AMyProjectile::AMyProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates=true;
	SetReplicateMovement(true);
	//初始化碰撞box
	CollisionBox=CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	SetRootComponent(CollisionBox);
	//设置碰撞
	CollisionBox->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	//Visibility 射线打到这个 CollisionBox
	CollisionBox->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECollisionChannel::ECC_WorldStatic, ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECC_SkeletalMesh, ECR_Block);
}

void AMyProjectile::ApplyExplosionDamage(const FVector& ImpactLocation)
{
	APawn* FiringPawn = GetInstigator();
	if (FiringPawn == nullptr) return;

	AController* Controller = FiringPawn->GetController();
	if (Controller == nullptr) return;

	UGameplayStatics::ApplyRadialDamageWithFalloff(
		this,
		Damage,
		MinimumDamage,
		ImpactLocation,
		DamageInnerRadius,
		DamageOuterRadius,
		DamageFalloff,
		UDamageType::StaticClass(),
		TArray<AActor*>(),
		this,
		Controller
	);
}

void AMyProjectile::BeginPlay()
{
	Super::BeginPlay();
	IgnoreActor(GetOwner());
	IgnoreActor(GetInstigator());
	if (Tracer && CollisionBox)
	{
		TracerComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			Tracer,
			CollisionBox,
			NAME_None,
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget,
			true
		);
	}
	if (bBindHitOnBeginPlay && HasAuthority() && CollisionBox)
	{
		CollisionBox->OnComponentHit.AddDynamic(this, &AMyProjectile::OnHit);
	}
	//CollisionBox->IgnoreActorWhenMoving() 可以用于忽略某些物体
}

bool AMyProjectile::BroadcastExplosionEffect(const FVector& ImpactLocation, const FVector& ImpactNormal)
{
	APawn* FiringPawn = GetInstigator();
	if (FiringPawn == nullptr) return false;

	AMyBlasterCharacter* FiringCharacter = Cast<AMyBlasterCharacter>(FiringPawn);
	if (FiringCharacter == nullptr) return false;

	UCombatComponent* Combat = FiringCharacter->GetCombat();
	if (Combat == nullptr) return false;

	Combat->BroadcastImpactEffect(ImpactNia, ImpactSound, ImpactLocation, ImpactNormal);
	return true;
}

void AMyProjectile::Destroyed()
{
	// Destroyed runs on replicated clients too, so keep impact effects here as the client-side callback.
	// Cache these before Super::Destroyed(), because components/world context may be torn down afterward.
	if (bPlayImpactEffectsOnDestroyed)
	{
		PlayImpactEffects(GetActorLocation(), GetActorRotation());
	}
	
	Super::Destroyed();
}

void AMyProjectile::DestroyTimerFinished()
{
	Destroy();
}

bool AMyProjectile::ExplodeDamageAndEffects(const FVector& ImpactLocation, const FVector& ImpactNormal,
	const FRotator& ImpactRotation)
{
	if (!HasAuthority() || bHasExploded) return false;

	bHasExploded = true;

	ApplyExplosionDamage(ImpactLocation);
	const bool bImpactBroadcasted = BroadcastExplosionEffect(ImpactLocation, ImpactNormal);

	StopProjectileMovementAndCollision();
	MulticastImpactEffects(ImpactLocation, ImpactRotation, !bImpactBroadcasted);

	return true;
}

void AMyProjectile::HandleImpactEffectsMulticast()
{
}

void AMyProjectile::IgnoreActor(AActor* ActorToIgnore)
{
	if (ActorToIgnore == nullptr) return;

	if (CollisionBox)
	{
		CollisionBox->IgnoreActorWhenMoving(ActorToIgnore, true);
	}
	if (APawn* PawnToIgnore = Cast<APawn>(ActorToIgnore))
	{
		PawnToIgnore->MoveIgnoreActorAdd(this);
	}
}

void AMyProjectile::MulticastImpactEffects_Implementation(const FVector_NetQuantize& ImpactLocation,
	FRotator ImpactRotation, bool bPlayFallbackImpact)
{
	if (bPlayFallbackImpact)
	{
		PlayImpactEffects(ImpactLocation, ImpactRotation);
	}

	if (MovementComponent)
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->Deactivate();
	}

	if (ProjectileMesh)
	{
		ProjectileMesh->SetVisibility(false);
	}

	if (CollisionBox)
	{
		CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (RocketTracerComponent)
	{
		RocketTracerComponent->Deactivate();
	}

	HandleImpactEffectsMulticast();
}

void AMyProjectile::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                          FVector NormalImpulse, const FHitResult& Hit)
{
	if (OtherActor == GetOwner() || OtherActor == GetInstigator())
	{
		return;
	}
	
	// Use the real blocking impact point so Destroyed() spawns replicated client effects at the stop location.
	if (Hit.bBlockingHit)
	{
		SetActorLocation(Hit.ImpactPoint);
	}

	// Stop any remaining movement/collision immediately; the visual tracer is cleaned up in Destroyed().
	StopProjectileMovementAndCollision();
	
	Destroy();
}

void AMyProjectile::PlayImpactEffects(const FVector& ImpactLocation, const FRotator& ImpactRotation)
{
	UWorld* World = GetWorld();

	// Stop the attached tracer before spawning the impact effect; otherwise the ribbon can look like a bounce.
	if (TracerComponent)
	{
		TracerComponent->DeactivateImmediate();
		TracerComponent->DestroyComponent();
		TracerComponent = nullptr;
	}

	/*
	UE_LOG(LogTemp, Verbose, TEXT("Projectile destroyed. Location=%s Sound=%s ImpactNia=%s"),
		*ImpactLocation.ToCompactString(),
		*GetNameSafe(ImpactSound),
		*GetNameSafe(ImpactNia));
*/
	if (World && ImpactSound)
	{
		UGameplayStatics::PlaySoundAtLocation(
			World,
			ImpactSound,
			ImpactLocation
		);
	}

	if (World && ImpactNia)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World,
			ImpactNia,
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

void AMyProjectile::SpawnTrailSystem()
{
	if (TrailSystem)
	{
		RocketTracerComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			TrailSystem,
			GetRootComponent(),
			FName(),
			GetActorLocation(),
			GetActorRotation(),
			EAttachLocation::KeepWorldPosition,
			false);
	}
}

void AMyProjectile::StartDestroyTimer()
{
	GetWorldTimerManager().SetTimer(DestroyTimerHandle, this,&AMyProjectile::DestroyTimerFinished, DestroyTime);
}

void AMyProjectile::StopProjectileMovementAndCollision()
{
	if (MovementComponent)
	{
		MovementComponent->StopMovementImmediately();
		MovementComponent->Deactivate();
	}
	if (CollisionBox)
	{
		CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SetActorEnableCollision(false);
	ForceNetUpdate();
}

void AMyProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
