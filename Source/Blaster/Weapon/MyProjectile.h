// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"
#include "MyProjectile.generated.h"

UCLASS()
class BLASTER_API AMyProjectile : public AActor
{
	GENERATED_BODY()
	
public:	
	AMyProjectile();
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	void IgnoreActor(AActor* ActorToIgnore);

	UPROPERTY(EditAnywhere)
	bool bUseServerRewind =false;

	FVector_NetQuantize StartLocation;
	FVector_NetQuantize100 InitVelocity;
	
	UPROPERTY(EditAnywhere)
	float InitSpeed = 15000.f;

	UPROPERTY(EditAnywhere)
	float Damage=20.f;

	UPROPERTY(EditAnywhere)
	float HeadShotDamageRatio = 4.f;
protected:
	virtual void BeginPlay() override;
	UFUNCTION()
	virtual void OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse,
	           const FHitResult& Hit);
	void StopProjectileMovementAndCollision();
	void PlayImpactEffects(const FVector& ImpactLocation, const FRotator& ImpactRotation);

	bool ExplodeDamageAndEffects(const FVector& ImpactLocation, const FVector& ImpactNormal, const FRotator& ImpactRotation);
	void ApplyExplosionDamage(const FVector& ImpactLocation);
	bool BroadcastExplosionEffect(const FVector& ImpactLocation, const FVector& ImpactNormal);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastImpactEffects(const FVector_NetQuantize& ImpactLocation, FRotator ImpactRotation, bool bPlayFallbackImpact);

	virtual void HandleImpactEffectsMulticast();

	virtual void DestroyTimerFinished();
	
	void StartDestroyTimer();
	bool bPlayImpactEffectsOnDestroyed = true;
	
	UPROPERTY(EditAnywhere, Category="Explosion")
	float MinimumDamage = 10.f;

	UPROPERTY(EditAnywhere, Category="Explosion")
	float DamageInnerRadius = 200.f;

	UPROPERTY(EditAnywhere, Category="Explosion")
	float DamageOuterRadius = 500.f;

	UPROPERTY(EditAnywhere, Category="Explosion")
	float DamageFalloff = 1.f;

	bool bHasExploded = false;
	//
	//Hit effect
	//
	UPROPERTY(EditAnywhere)
	class UNiagaraSystem* ImpactNia;

	UPROPERTY(EditAnywhere)
	USoundCue* ImpactSound;

	UPROPERTY(EditAnywhere)
	UBoxComponent* CollisionBox;
	
	UPROPERTY(VisibleAnywhere)
	class UProjectileMovementComponent* MovementComponent;

	UPROPERTY()
	class UNiagaraComponent* RocketTracerComponent;
	UPROPERTY(EditAnywhere)
	UNiagaraSystem* TrailSystem;
	void SpawnTrailSystem();

	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* ProjectileMesh;

	bool bBindHitOnBeginPlay = true;
private: 
	//
	//我想说的是，这个投射物class基本上跟weapon类分开，耦合性讲道理不高
	//


	//
	//Tracer
	//
	
	//
	//资产特效
	//
	UPROPERTY(EditAnywhere)
	class UNiagaraSystem* Tracer;

	//
	//绑定粒子的组件
	//
	UPROPERTY()
	class UNiagaraComponent* TracerComponent;


	UPROPERTY(EditAnywhere)
	float DestroyTime=1.f;
	FTimerHandle DestroyTimerHandle;
};
