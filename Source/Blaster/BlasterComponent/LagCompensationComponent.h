#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

class AMyBlasterCharacter;
class ABlasterPlayerController;

#include "LagCompensationComponent.generated.h"

USTRUCT(Blueprintable)
struct FBoxInformation
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	FVector Location;

	UPROPERTY()
	FRotator Rotation;

	UPROPERTY()
	FVector BoxExtent;
};


USTRUCT(Blueprintable)
struct FFramePackage
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	float Time;

	UPROPERTY()
	TMap<FName, FBoxInformation> HitBoxInfo;

	UPROPERTY()
	AMyBlasterCharacter* Character; 
};

USTRUCT()
struct FServerRewindResult
{
	GENERATED_BODY()
	UPROPERTY()
	bool bHitConfirmed;
	UPROPERTY()
	bool bHeadShot;
};

USTRUCT()
struct FShotGunServerRewindResult
{
	GENERATED_BODY()
	UPROPERTY()
	TMap<AMyBlasterCharacter*, uint32> HeadShotTime;
	UPROPERTY()
	TMap<AMyBlasterCharacter*, uint32> BodyShotTime;
};


UCLASS()
class BLASTER_API ULagCompensationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	friend class AMyBlasterCharacter;
	ULagCompensationComponent();
	void DebugShowFramePackage(const FFramePackage& Package, const FColor& Color);
	FServerRewindResult ServerSideRewind(AMyBlasterCharacter* HitCharacter,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize& HitLocation,
		float HitTime);
	UFUNCTION(Server, Reliable)
	void ServerScoreRequest(AMyBlasterCharacter* HitCharacter,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize& HitLocation,
		float HitTime,
		class AWeapon* HitCauser);


	FShotGunServerRewindResult ShotGunServerRewind(const TArray<AMyBlasterCharacter*>& HitCharacters,
		const FVector_NetQuantize& TraceStart,
		const TArray<FVector_NetQuantize>& HitLocations,
		float HitTime);

	UFUNCTION(Server, Reliable)
	void ShotGunServerScoreRequest(const TArray<AMyBlasterCharacter*>& HitCharacters,
		const FVector_NetQuantize& TraceStart,
		const TArray<FVector_NetQuantize>& HitLocations,
		float HitTime,
		class AWeapon* HitCauser);

	FServerRewindResult ProjectileServerSideRewind(AMyBlasterCharacter* HitCharacter,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize100& InitialVelocity,
		float HitTime);

	UFUNCTION(Server, Reliable)
	void ProjectileServerScoreRequest(AMyBlasterCharacter* HitCharacter,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize100& InitialVelocity,
		float HitTime);
protected:
	virtual void BeginPlay() override;

	FFramePackage InterpBetweenFrame(const FFramePackage& OldFrame,
		const FFramePackage& YoungFrame,
		const float HitTime);
	FServerRewindResult ConfirmHit(
		const FFramePackage& FramePackage,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize& HitLocation);
	void CacheCharacterBoxInfo(AMyBlasterCharacter* Character,FFramePackage& OutPackageFrame);
	void MoveCharacterBoxInfo(AMyBlasterCharacter* Character,const FFramePackage& InPackageFrame);
	void ResetCharacterBoxInfo(AMyBlasterCharacter* Character,const FFramePackage& InPackageFrame);
	void EnableCharacterMeshCollision(AMyBlasterCharacter* Character,ECollisionEnabled::Type CollisionType);
	FFramePackage GetFrameToCheck(AMyBlasterCharacter* HitCharacter,float HitTime);
	
	/*
	ShotGun Rewind
	 */
	FShotGunServerRewindResult ShotGunConfirmHit(const TArray<FFramePackage>& FramePackages,
		const FVector_NetQuantize& TraceStart,
		const TArray<FVector_NetQuantize>& HitLocations
		);

	FServerRewindResult ProjectileConfirmHit(
		const FFramePackage& FramePackage,
		const FVector_NetQuantize& TraceStart,
		const FVector_NetQuantize100& InitialVelocity,
		float HitTime);
public:
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	UPROPERTY()
	AMyBlasterCharacter* Character;

	UPROPERTY()
	ABlasterPlayerController* Controller;

	UFUNCTION()
	void SaveFramePackage(FFramePackage& Package);
	void SaveFramePackageHistory();
	TDoubleLinkedList<FFramePackage>FrameHistory;

	UPROPERTY(EditAnywhere)
	float MaxRecordTime = 2.f;
};
