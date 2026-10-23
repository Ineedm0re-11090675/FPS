// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Bullet.generated.h"

UCLASS()
class BLASTER_API ABullet : public AActor
{
	GENERATED_BODY()
	
public:	
	ABullet();
protected:
	virtual void BeginPlay() override;
	UFUNCTION()
	virtual void OnHit(UPrimitiveComponent* HitComp,AActor* OtherActor, UPrimitiveComponent* OtherComp,FVector NormalImpulse,const FHitResult& Hit);
private:
	
	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* StaticMesh;
	
	UPROPERTY(EditAnywhere)
	float ShellEjectionImpulse;
	
	UPROPERTY(EditAnywhere)
	class USoundCue* ShellSound;

	bool bHasHit = false;
};
