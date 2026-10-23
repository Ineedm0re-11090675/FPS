// Fill out your copyright notice in the Description page of Project Settings.


#include "ProjectileWeapon.h"
#include "MyProjectile.h"
#include "Engine/SkeletalMeshSocket.h"

void AProjectileWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget);
	//
	//弹丸的控制权都在server上，client能看到需要复制
	//
	APawn* InstigatorPawn=Cast<APawn>(GetOwner());
	const USkeletalMeshSocket* FireFlashSocket= GetWeaponMesh()->GetSocketByName(FName("FireFlashSocket"));
	UWorld* World = GetWorld();
	if (InstigatorPawn == nullptr || FireFlashSocket == nullptr || World == nullptr) return;
	
	if (FireFlashSocket && World)
	{
		FTransform SocketTransform=FireFlashSocket->GetSocketTransform((GetWeaponMesh()));
		//追踪吧，如果你再甩枪，有点像飘逸，但是以子弹的速度来看很合理
		//从socket到标准十字
		const FVector SocketLocation = SocketTransform.GetLocation();
		FVector ToTarget=HitTarget-SocketLocation;
		FVector TargetDirection = ToTarget.GetSafeNormal();
		if (TargetDirection.IsNearlyZero())
		{
			TargetDirection = GetActorForwardVector();
		}
		const FVector SpawnLocation = SocketLocation + TargetDirection * 50.f;
		//然后再变成Rotator
		FRotator ToTargetRot=TargetDirection.Rotation();

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Owner=GetOwner();
		SpawnParameters.Instigator=InstigatorPawn;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		auto SpawnProjectile = [&](TSubclassOf<AMyProjectile> ClassToSpawn, bool bShouldUseServerRewind, bool bShouldSetRewindData, bool bShouldSetDamage)
		{
				if (ClassToSpawn == nullptr)
				{
					UE_LOG(LogTemp, Verbose, TEXT("[ProjectileWeapon::fire] Missing projectile class. Weapon=%s Owner=%s"),
						*GetNameSafe(this),
						*GetNameSafe(GetOwner())
					);
				return static_cast<AMyProjectile*>(nullptr);
			}

			AMyProjectile* SpawnedProjectile = World->SpawnActor<AMyProjectile>(
				ClassToSpawn,
				SpawnLocation,
				ToTargetRot,
				SpawnParameters
			);

				UE_LOG(LogTemp, Verbose, TEXT("[ProjectileWeapon::fire] Auth=%d PawnAuth=%d PawnLocal=%d bSSR=%d Class=%s Socket=%s Spawn=%s Owner=%s Instigator=%s"),
					HasAuthority(),
				InstigatorPawn->HasAuthority(),
				InstigatorPawn->IsLocallyControlled(),
				bShouldUseServerRewind,
				*GetNameSafe(ClassToSpawn.Get()),
				*SocketLocation.ToCompactString(),
				*SpawnLocation.ToCompactString(),
				*GetNameSafe(GetOwner()),
				*GetNameSafe(InstigatorPawn)
			);

			if (SpawnedProjectile == nullptr) return static_cast<AMyProjectile*>(nullptr);

			SpawnedProjectile->bUseServerRewind = bShouldUseServerRewind;
			SpawnedProjectile->IgnoreActor(GetOwner());
			SpawnedProjectile->IgnoreActor(InstigatorPawn);
			SpawnedProjectile->IgnoreActor(this);

			if (bShouldSetRewindData)
			{
				SpawnedProjectile->StartLocation = SpawnLocation;
				SpawnedProjectile->InitVelocity = TargetDirection * SpawnedProjectile->InitSpeed;
			}
			if (bShouldSetDamage)
			{
				SpawnedProjectile->Damage = Damage;
				SpawnedProjectile->HeadShotDamageRatio =HeadShotDamageRatio;
			}
			return SpawnedProjectile;
		};

		if (bUseServerRewind)
		{
			if (InstigatorPawn->HasAuthority())
			{
				if (InstigatorPawn->IsLocallyControlled())
				{
					SpawnProjectile(ProjectileClass, false, false, true);
				}
				else
				{
					SpawnProjectile(ServerRewindProjectileClass, true, false, false);
				}
			}else
			{
				if (InstigatorPawn->IsLocallyControlled())
				{
					SpawnProjectile(ServerRewindProjectileClass, true, true, true);
				}
				else
				{
					SpawnProjectile(ServerRewindProjectileClass, false, false, false);
				}
			}
		}else
		{
			if (InstigatorPawn->HasAuthority())
			{
				SpawnProjectile(ProjectileClass, false, false, true);
			}
		}
		/*
		一个client开火，他生成的确实是不需要复制的子弹，因为复制了也没用，但是要SSR用来计算。然后这个fire触发RPC，server再出发mult，所以在其他client和server上发射不复制，也不SSR的子弹。
		一个server开火，肯定是不用SSR的，但是复制是为了什么呢？我忘了是不是rep的物体可以让client端也看到？
		这样的话也不用RPC，就rep让其他client看到特效就行？
		*/
	}
}
