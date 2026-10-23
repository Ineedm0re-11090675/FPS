// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Weapon.h"
#include "ProjectileWeapon.generated.h"

/**
 * 
 */
UCLASS()
class BLASTER_API AProjectileWeapon : public AWeapon
{
	GENERATED_BODY()
private:
	// class AProjectile 是前向声明，可以减少头文件依赖；
	// 但在 UPROPERTY / 反射系统里，UHT 可能仍然需要 include 对应头文件。
	UPROPERTY(EditAnywhere)
	TSubclassOf<class AMyProjectile> ProjectileClass;

	UPROPERTY(EditAnywhere)
	TSubclassOf< AMyProjectile> ServerRewindProjectileClass;
public:
	virtual void Fire(const FVector& HitTarget) override;
};
