#include "LagCompensationComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Components/BoxComponent.h"
#include "DrawDebugHelpers.h"
#include "Blaster/Blaster.h"
#include "Blaster/Weapon/Weapon.h"
#include "Kismet/GameplayStatics.h"

void ULagCompensationComponent::BeginPlay()
{
	Super::BeginPlay();
}

FFramePackage ULagCompensationComponent::InterpBetweenFrame(const FFramePackage& OldFrame,
	const FFramePackage& YoungFrame, const float HitTime)
{
	const float DeltaTime =YoungFrame.Time - OldFrame.Time;
	FFramePackage InterpPackage;
	InterpPackage.Time = HitTime;
	InterpPackage.Character = YoungFrame.Character ? YoungFrame.Character : OldFrame.Character;

	if (DeltaTime <= KINDA_SMALL_NUMBER)
	{
		InterpPackage.HitBoxInfo = OldFrame.HitBoxInfo;
		return InterpPackage;
	}

	const float InterpFraction = FMath::Clamp((HitTime - OldFrame.Time )/DeltaTime,0.f,1.f);

	for (auto& YoungerPair :YoungFrame.HitBoxInfo)
	{
		const FName& BoxName = YoungerPair.Key;

		const FBoxInformation* OldBox = OldFrame.HitBoxInfo.Find(BoxName);
		const FBoxInformation* YoungBox = YoungFrame.HitBoxInfo.Find(BoxName);
		if (OldBox == nullptr || YoungBox == nullptr) continue;

		FBoxInformation InterpBoxInfo;

		//Wht 1.f? 因为插值 = DeltaTime*InterpSpeed，我们只要InterpFraction*1即可
		InterpBoxInfo.Location = FMath::VInterpTo(OldBox->Location,YoungBox->Location,1.f,InterpFraction);
		InterpBoxInfo.Rotation = FMath::RInterpTo(OldBox->Rotation,YoungBox->Rotation,1.f,InterpFraction);
		InterpBoxInfo.BoxExtent = YoungBox->BoxExtent;

		InterpPackage.HitBoxInfo.Add(BoxName,InterpBoxInfo);
	}
	return InterpPackage;
}

FServerRewindResult ULagCompensationComponent::ConfirmHit(const FFramePackage& FramePackage,const FVector_NetQuantize& TraceStart, const FVector_NetQuantize& HitLocation)
{
	if (FramePackage.Character == nullptr) return FServerRewindResult();

	FFramePackage CurrentFrame;
	CacheCharacterBoxInfo(FramePackage.Character,CurrentFrame);
	MoveCharacterBoxInfo(FramePackage.Character,FramePackage);
	EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::NoCollision);
	/*
	 *Detect HeadShot
	 */
	UBoxComponent* const* HeadComponentPtr = FramePackage.Character->HitCollisionBox.Find(FName("head"));
	if (HeadComponentPtr == nullptr || *HeadComponentPtr == nullptr)
	{
		ResetCharacterBoxInfo(FramePackage.Character,CurrentFrame);
		EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::QueryAndPhysics);
		return FServerRewindResult(false,false);
	}
	UBoxComponent* HeadComponent = *HeadComponentPtr;
	HeadComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HeadComponent->SetCollisionResponseToChannel(ECC_HitCollisionBox,ECR_Block);

	FHitResult HitResult;
	UWorld* World =GetWorld();
	const FVector TraceEnd =TraceStart + (HitLocation- TraceStart) *1.25f;
	if (World)
	{
		World->LineTraceSingleByChannel(
			HitResult,
			TraceStart,
			TraceEnd,
			ECC_HitCollisionBox
			);
			if (HitResult.bBlockingHit)
			{
				ResetCharacterBoxInfo(FramePackage.Character,CurrentFrame);
				EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::QueryAndPhysics);
				return FServerRewindResult(true,true);
		}else
		{
			for (auto& BoxPair : FramePackage.Character->HitCollisionBox)
			{
				if (BoxPair.Value != nullptr)
				{
						BoxPair.Value->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
						BoxPair.Value->SetCollisionResponseToChannel(ECC_HitCollisionBox,ECR_Block);
				}
			}
			World->LineTraceSingleByChannel(
			HitResult,
			TraceStart,
			TraceEnd,
				ECC_HitCollisionBox
			);
				if (HitResult.bBlockingHit)
				{
					ResetCharacterBoxInfo(FramePackage.Character,CurrentFrame);
					EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::QueryAndPhysics);
					return FServerRewindResult(true,false);
			}
		}
	}
	ResetCharacterBoxInfo(FramePackage.Character,CurrentFrame);
	EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::QueryAndPhysics);
	return FServerRewindResult(false,false);
}
FShotGunServerRewindResult ULagCompensationComponent::ShotGunConfirmHit(const TArray<FFramePackage>& FramePackages,
	const FVector_NetQuantize& TraceStart, const TArray<FVector_NetQuantize>& HitLocations)
{

	for (auto& FramePackage : FramePackages)
	{
		if (FramePackage.Character == nullptr) return FShotGunServerRewindResult();
	}
	
	FShotGunServerRewindResult RewindResult;
	TArray<FFramePackage> CurrentFramePackages;
	for (auto& FramePackage : FramePackages)
	{
			FFramePackage CurrentFrame;
			CurrentFrame.Character=FramePackage.Character; 
			CacheCharacterBoxInfo(FramePackage.Character,CurrentFrame);
			MoveCharacterBoxInfo(FramePackage.Character,FramePackage);
			EnableCharacterMeshCollision(FramePackage.Character,ECollisionEnabled::NoCollision);
			CurrentFramePackages.Add(CurrentFrame);
		}
	for (auto& FramePackage : FramePackages)
	{
		if (UBoxComponent* const* HeadComponent = FramePackage.Character->HitCollisionBox.Find(FName("head")))
		{
			if (*HeadComponent)
			{
				(*HeadComponent)->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				(*HeadComponent)->SetCollisionResponseToChannel(ECC_HitCollisionBox,ECR_Block);
			}
		}
	}
	UWorld* World =GetWorld();
	if (World)
	{
		for (auto& HitLocation : HitLocations)
		{
			FHitResult ShotGunHitResult;
			const FVector TraceEnd =TraceStart + (HitLocation- TraceStart) *1.25f;
			World->LineTraceSingleByChannel(
				ShotGunHitResult,
				TraceStart,
				TraceEnd,
				ECC_HitCollisionBox 
				);
				AMyBlasterCharacter* HitCharacter = Cast<AMyBlasterCharacter>(ShotGunHitResult.GetActor());
				if (HitCharacter != nullptr)
				{
					if (RewindResult.HeadShotTime.Contains(HitCharacter))
					{
						RewindResult.HeadShotTime[HitCharacter]++;
				}else
				{
					RewindResult.HeadShotTime.Emplace(HitCharacter,1);
				}
			}
		}
	}
	for (auto& FramePackage : FramePackages)
	{
		for (auto& BoxPair : FramePackage.Character->HitCollisionBox)
		{
			if (BoxPair.Value != nullptr)
			{
				BoxPair.Value->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				BoxPair.Value->SetCollisionResponseToChannel(ECC_HitCollisionBox,ECR_Block);
			}
			}
			if (UBoxComponent* const* HeadComponent = FramePackage.Character->HitCollisionBox.Find(FName("head")))
			{
				if (*HeadComponent)
				{
					(*HeadComponent)->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				}
			}
		}

	
	if (World)
	{
		for (auto& HitLocation : HitLocations)
		{
			FHitResult ShotGunHitResult;
			const FVector TraceEnd =TraceStart + (HitLocation- TraceStart) *1.25f;
			World->LineTraceSingleByChannel(
				ShotGunHitResult,
				TraceStart,
				TraceEnd,
				ECC_HitCollisionBox
				);
				AMyBlasterCharacter* HitCharacter = Cast<AMyBlasterCharacter>(ShotGunHitResult.GetActor());
				if (HitCharacter != nullptr)
				{
					if (RewindResult.BodyShotTime.Contains(HitCharacter))
					{
						RewindResult.BodyShotTime[HitCharacter]++;
				}else
				{
					RewindResult.BodyShotTime.Emplace(HitCharacter,1);
				}
			}
		}
	}
	for (auto& CurrentFrame : CurrentFramePackages)
	{
		ResetCharacterBoxInfo(CurrentFrame.Character,CurrentFrame);
		EnableCharacterMeshCollision(CurrentFrame.Character,ECollisionEnabled::QueryAndPhysics);
	}
	return RewindResult;
}

FServerRewindResult ULagCompensationComponent::ProjectileConfirmHit(const FFramePackage& FramePackage,
	const FVector_NetQuantize& TraceStart, const FVector_NetQuantize100& InitialVelocity, float HitTime)
{
	if (FramePackage.Character == nullptr) return FServerRewindResult();

	FFramePackage CurrentFrame;
	CacheCharacterBoxInfo(FramePackage.Character, CurrentFrame);
	MoveCharacterBoxInfo(FramePackage.Character, FramePackage);
	EnableCharacterMeshCollision(FramePackage.Character, ECollisionEnabled::NoCollision);

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		ResetCharacterBoxInfo(FramePackage.Character, CurrentFrame);
		EnableCharacterMeshCollision(FramePackage.Character, ECollisionEnabled::QueryAndPhysics);
		return FServerRewindResult();
	}

	FPredictProjectilePathParams PathParams;
	PathParams.bTraceWithChannel = true;
	PathParams.bTraceWithCollision = true;
	PathParams.MaxSimTime = MaxRecordTime;
	PathParams.LaunchVelocity = InitialVelocity;
	PathParams.StartLocation = TraceStart;
	PathParams.SimFrequency = 15.f;
	PathParams.ProjectileRadius = 5.f;
	PathParams.TraceChannel = ECC_HitCollisionBox;
	PathParams.ActorsToIgnore.Add(GetOwner());
	PathParams.DrawDebugType = EDrawDebugTrace::None;

	FPredictProjectilePathResult PathResult;

	if (UBoxComponent* const* HeadComponent = FramePackage.Character->HitCollisionBox.Find(FName("head")))
	{
		if (*HeadComponent)
		{
			(*HeadComponent)->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			(*HeadComponent)->SetCollisionResponseToChannel(ECC_HitCollisionBox, ECR_Block);
		}
	}

	UGameplayStatics::PredictProjectilePath(this, PathParams, PathResult);
	if (PathResult.HitResult.bBlockingHit)
	{
		ResetCharacterBoxInfo(FramePackage.Character, CurrentFrame);
		EnableCharacterMeshCollision(FramePackage.Character, ECollisionEnabled::QueryAndPhysics);
		return FServerRewindResult(true, true);
	}

	for (auto& BoxPair : FramePackage.Character->HitCollisionBox)
	{
		if (BoxPair.Value != nullptr)
		{
			BoxPair.Value->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			BoxPair.Value->SetCollisionResponseToChannel(ECC_HitCollisionBox, ECR_Block);
		}
	}

	if (UBoxComponent* const* HeadComponent = FramePackage.Character->HitCollisionBox.Find(FName("head")))
	{
		if (*HeadComponent)
		{
			(*HeadComponent)->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	UGameplayStatics::PredictProjectilePath(this, PathParams, PathResult);
	if (PathResult.HitResult.bBlockingHit)
	{
		ResetCharacterBoxInfo(FramePackage.Character, CurrentFrame);
		EnableCharacterMeshCollision(FramePackage.Character, ECollisionEnabled::QueryAndPhysics);
		return FServerRewindResult(true, false);
	}

	ResetCharacterBoxInfo(FramePackage.Character, CurrentFrame);
	EnableCharacterMeshCollision(FramePackage.Character, ECollisionEnabled::QueryAndPhysics);
	return FServerRewindResult(false, false);
}

void ULagCompensationComponent::CacheCharacterBoxInfo(AMyBlasterCharacter* Character, FFramePackage& OutPackageFrame)
{
	if (Character == nullptr) return;
	for (auto& HitBox : Character->HitCollisionBox)
	{
		if (HitBox.Value != nullptr)
		{
			FBoxInformation BoxInfo;
			BoxInfo.Location = HitBox.Value->GetComponentLocation();
			BoxInfo.Rotation = HitBox.Value->GetComponentRotation();
			BoxInfo.BoxExtent =HitBox.Value->GetScaledBoxExtent();
			OutPackageFrame.HitBoxInfo.Add(HitBox.Key,BoxInfo);
		}
	}
}

void ULagCompensationComponent::MoveCharacterBoxInfo(AMyBlasterCharacter* Character,const FFramePackage& InPackageFrame)
{
	if (Character == nullptr) return;
	for (auto& HitBox : Character->HitCollisionBox)
	{
		if (HitBox.Value != nullptr)
		{
			const FBoxInformation* BoxInfo = InPackageFrame.HitBoxInfo.Find(HitBox.Key);
			if (BoxInfo == nullptr) continue;

			HitBox.Value->SetWorldLocation(BoxInfo->Location);
			HitBox.Value->SetWorldRotation(BoxInfo->Rotation);
			HitBox.Value->SetBoxExtent(BoxInfo->BoxExtent);
		}
	}
}

void ULagCompensationComponent::ResetCharacterBoxInfo(AMyBlasterCharacter* Character,const FFramePackage& InPackageFrame)
{
	if (Character == nullptr) return;
	for (auto& HitBox : Character->HitCollisionBox)
	{
		if (HitBox.Value != nullptr)
		{
			const FBoxInformation* BoxInfo = InPackageFrame.HitBoxInfo.Find(HitBox.Key);
			if (BoxInfo == nullptr) continue;

			HitBox.Value->SetWorldLocation(BoxInfo->Location);
			HitBox.Value->SetWorldRotation(BoxInfo->Rotation);
			HitBox.Value->SetBoxExtent(BoxInfo->BoxExtent);
			HitBox.Value->SetCollisionEnabled(ECollisionEnabled::NoCollision); 
		}
	}
}

void ULagCompensationComponent::EnableCharacterMeshCollision(AMyBlasterCharacter* Character,
	ECollisionEnabled::Type CollisionType)
{
	if (Character && Character->GetMesh())
	{
		Character->GetMesh()->SetCollisionEnabled(CollisionType);
	}
}

FFramePackage ULagCompensationComponent::GetFrameToCheck(AMyBlasterCharacter* HitCharacter,float HitTime)
{
	bool bReturn =
		HitCharacter==nullptr ||
			HitCharacter->GetLagCompensation() == nullptr ||
				HitCharacter->GetLagCompensation()->FrameHistory.GetHead() == nullptr ||
					HitCharacter->GetLagCompensation()->FrameHistory.GetTail() == nullptr ;
	if (bReturn) return FFramePackage();

	FFramePackage PackageToCheck;
	bool bShouldInterp =true;
	const TDoubleLinkedList<FFramePackage>& History = HitCharacter->GetLagCompensation()->FrameHistory;
	const float OldestHistoryTime =History.GetTail()->GetValue().Time;
	const float YoungestHistoryTime =History.GetHead()->GetValue().Time;
	if (HitTime < OldestHistoryTime)
	{
		return FFramePackage();
	}
	if (HitTime >= YoungestHistoryTime)
	{
		PackageToCheck = History.GetHead()->GetValue();
		bShouldInterp = false;
	}
	if (HitTime == OldestHistoryTime)
	{
		PackageToCheck = History.GetTail()->GetValue();
		bShouldInterp = false;
	}
	TDoubleLinkedList<FFramePackage>::TDoubleLinkedListNode* Younger = History.GetHead();
	TDoubleLinkedList<FFramePackage>::TDoubleLinkedListNode* Older = Younger;

	while (Older->GetValue().Time >HitTime)
	{
		//回滚，一直滚到Older和younger夹住为止
		//这里我也在思考能不能用二分法，用空间换时间？看后期是内存问题还是运行问题吧
		if (Older->GetNextNode() == nullptr) break;
		Older = Older->GetNextNode();
		if (Older->GetValue().Time > HitTime)
		{
			Younger=Older;
		}
	}
	if (Older->GetValue().Time == HitTime)
	{
		PackageToCheck = Older->GetValue();
		bShouldInterp = false;
	}
	if (bShouldInterp)
	{
		//interp
		PackageToCheck = InterpBetweenFrame(Older->GetValue(),Younger->GetValue(),HitTime);
	}
	PackageToCheck.Character = HitCharacter; 
	return PackageToCheck;
}

FServerRewindResult ULagCompensationComponent::ServerSideRewind(AMyBlasterCharacter* HitCharacter,
	const FVector_NetQuantize& TraceStart,
	const FVector_NetQuantize& HitLocation,
	float HitTime)
{
	FFramePackage FrameToCheck = GetFrameToCheck(HitCharacter, HitTime);
	return ConfirmHit(FrameToCheck,TraceStart,HitLocation);
}

FShotGunServerRewindResult ULagCompensationComponent::ShotGunServerRewind(
	const TArray<AMyBlasterCharacter*>& HitCharacters, const FVector_NetQuantize& TraceStart,
	const TArray<FVector_NetQuantize>& HitLocations, float HitTime)
{
	TArray<FFramePackage> FrameToChecks;
	for (AMyBlasterCharacter* HitCharacter : HitCharacters)
	{
		FrameToChecks.Add(GetFrameToCheck(HitCharacter,HitTime));
	}
	return ShotGunConfirmHit(FrameToChecks,TraceStart,HitLocations);
}

FServerRewindResult ULagCompensationComponent::ProjectileServerSideRewind(AMyBlasterCharacter* HitCharacter,
	const FVector_NetQuantize& TraceStart, const FVector_NetQuantize100& InitialVelocity, float HitTime)
{
	FFramePackage FrameToCheck = GetFrameToCheck(HitCharacter, HitTime);
	return ProjectileConfirmHit(FrameToCheck, TraceStart, InitialVelocity, HitTime);
}



void ULagCompensationComponent::ServerScoreRequest_Implementation(AMyBlasterCharacter* HitCharacter,
                                                                 const FVector_NetQuantize& TraceStart, const FVector_NetQuantize& HitLocation, float HitTime,
                                                                 class AWeapon* HitCauser)
{
	FServerRewindResult Confirm = ServerSideRewind(HitCharacter,TraceStart,HitLocation,HitTime);

	if (Confirm.bHitConfirmed && HitCharacter && HitCauser && Character && Character->Controller)
	{
		const float Damage = Confirm.bHeadShot ? HitCauser->GetDamage() * HitCauser->GetHeadShotRatio() :HitCauser->GetDamage();
		UGameplayStatics::ApplyDamage(
			HitCharacter,
			Damage,
			Character->Controller,
			HitCauser,
			UDamageType::StaticClass()
			);
	}
}
void ULagCompensationComponent::ShotGunServerScoreRequest_Implementation(const TArray<AMyBlasterCharacter*>& HitCharacters,
		const FVector_NetQuantize& TraceStart,
		const TArray<FVector_NetQuantize>& HitLocations,
		float HitTime,
		class AWeapon* HitCauser)
{
	FShotGunServerRewindResult Confirm = ShotGunServerRewind(HitCharacters,TraceStart,HitLocations,HitTime);
	for (auto& HitCharacter : HitCharacters)
	{
		if (Character == nullptr || Character->Controller == nullptr || HitCauser == nullptr) return;
		if (HitCharacter == nullptr) continue;
		float TotalDamage = 0.f;
		const float HeadShotDamage =HitCauser->GetDamage() * HitCauser->GetHeadShotRatio();
		if (Confirm.HeadShotTime.Contains(HitCharacter))
		{
			TotalDamage+= Confirm.HeadShotTime[HitCharacter] *  HeadShotDamage;
		}
		if (Confirm.BodyShotTime.Contains(HitCharacter))
		{
			TotalDamage+= Confirm.BodyShotTime[HitCharacter] * HitCauser->GetDamage();
		}
		if (TotalDamage <= 0.f) continue;
		UGameplayStatics::ApplyDamage(
			HitCharacter,
			TotalDamage,
			Character->Controller,
			HitCauser,
			UDamageType::StaticClass()
			);
	}
}

void ULagCompensationComponent::ProjectileServerScoreRequest_Implementation(AMyBlasterCharacter* HitCharacter,
	const FVector_NetQuantize& TraceStart, const FVector_NetQuantize100& InitialVelocity, float HitTime)
{
	FServerRewindResult Confirm = ProjectileServerSideRewind(HitCharacter, TraceStart, InitialVelocity, HitTime);
	if (Confirm.bHitConfirmed && HitCharacter && Character && Character->Controller && Character->GetEquippedWeapon())
	{
		const float Damage = Confirm.bHeadShot ? Character->GetEquippedWeapon()->GetDamage() * Character->GetEquippedWeapon()->GetHeadShotRatio() : Character->GetEquippedWeapon()->GetDamage();
		UGameplayStatics::ApplyDamage(
			HitCharacter,
			Damage,
			Character->Controller,
			Character->GetEquippedWeapon(),
			UDamageType::StaticClass()
			);
	}
}


void ULagCompensationComponent::SaveFramePackage(FFramePackage& Package)
{
	if (Character == nullptr)
	{
		Character = Cast<AMyBlasterCharacter>(GetOwner());
	}
	if (Character == nullptr || GetWorld() == nullptr) return;
	
	Package.Time = GetWorld()->GetTimeSeconds();
	Package.Character = Character;
	for (auto& BoxPair : Character->HitCollisionBox)
	{
		if (BoxPair.Value == nullptr) continue;
		FBoxInformation BoxInfo;
		BoxInfo.Location = BoxPair.Value->GetComponentLocation();
		BoxInfo.Rotation = BoxPair.Value->GetComponentRotation();
		BoxInfo.BoxExtent = BoxPair.Value->GetScaledBoxExtent();
		Package.HitBoxInfo.Add(BoxPair.Key, BoxInfo);
	}
}
void ULagCompensationComponent::SaveFramePackageHistory()
{
	if (FrameHistory.Num() <=1)
	{
		FFramePackage Package;
		SaveFramePackage(Package);
		FrameHistory.AddHead(Package);
	}else
	{
		float HistoryLen = FrameHistory.GetHead()->GetValue().Time - FrameHistory.GetTail()->GetValue().Time;
		while (HistoryLen > MaxRecordTime)
		{
			FrameHistory.RemoveNode(FrameHistory.GetTail());
			HistoryLen = FrameHistory.GetHead()->GetValue().Time - FrameHistory.GetTail()->GetValue().Time;
		}
		FFramePackage Package;
		SaveFramePackage(Package);
		FrameHistory.AddHead(Package);

		//DebugShowFramePackage(Package,FColor::Red);
	}
}

void ULagCompensationComponent::TickComponent(float DeltaTime, enum ELevelTick TickType,
											  FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (Character != nullptr && Character->HasAuthority())
	{
		SaveFramePackageHistory();
	}
}
ULagCompensationComponent::ULagCompensationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void ULagCompensationComponent::DebugShowFramePackage(const FFramePackage& Package, const FColor& Color)
{
	if (GetWorld() == nullptr) return;
	for (const auto& HitBox : Package.HitBoxInfo)
	{
		DrawDebugBox(
			GetWorld(),
			HitBox.Value.Location,
			HitBox.Value.BoxExtent,
			FQuat(HitBox.Value.Rotation),
			Color,
			false,
			4.f
			);
	}
}
