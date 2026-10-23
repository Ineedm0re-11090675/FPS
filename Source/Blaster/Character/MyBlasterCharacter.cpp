// Fill out your copyright notice in the Description page of Project Settings.


#include "MyBlasterCharacter.h"

#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/WidgetComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "../HUD/OverHeadWidget.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Weapon/Weapon.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Net/UnrealNetwork.h"
#include "Blaster/Blaster.h"
#include "Blaster/BuffComponent/BuffComponent.h"
#include "Blaster/Gamemode/BlasterGameMode.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "UObject/CoreNetTypes.h"
#include "kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundCue.h"
#include "Components/BoxComponent.h"
#include "Blaster/BlasterComponent/LagCompensationComponent.h"
#include "Blaster/GameState/BlasterGameState.h"

AMyBlasterCharacter::AMyBlasterCharacter()
{
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	//绑在胶囊体上
	CameraBoom->SetupAttachment(GetMesh());
	CameraBoom->TargetArmLength = 600.0f;
	//这里和之前的课程吻合了，SpringArm开启Pawn跟随旋转，可以跟着Mesh转动
	CameraBoom->bUsePawnControlRotation = true;
	//挂在SpringArm上，作为一个Socket 
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	bUseControllerRotationYaw = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;

	OverHeadWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("OverheadWidget"));
	OverHeadWidget->SetupAttachment(RootComponent);

	Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("CombatComponent"));
	Combat->SetIsReplicated(true);

	Buff =CreateDefaultSubobject<UBuffComponent>(TEXT("BuffComponent"));
	Buff->SetIsReplicated(true);	

	LagCompensation = CreateDefaultSubobject<ULagCompensationComponent>(TEXT("LagCompensationComponent"));
	
	GetCharacterMovement()->NavAgentProps.bCanCrouch = true;

	TurningInPlace = ETurningInPlace::ETIP_Noturning;
	//网络更新速度
	SetNetUpdateFrequency(66.f);
	SetMinNetUpdateFrequency(33.f);
	//可以设置旋转速度
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 0.0f, 850.0f);

	//
	//CollisionSettings
	//
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECR_Ignore);
	GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECR_Ignore);
	GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECR_Block);
	GetMesh()->SetCollisionObjectType(ECC_SkeletalMesh);

	//
	//Dissolve
	//
	DissolveTimeline = CreateDefaultSubobject<UTimelineComponent>(TEXT("DissolveTimelineComponent"));

	/*
	 *Grenade
	 */
	AttachedGrenade = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AttachedGrenade"));
	AttachedGrenade->SetupAttachment(GetMesh(),FName("GrenadeSocket"));
	AttachedGrenade->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	/*
	 *Hit Boxes for client server rewind
	 */
	auto CreateHitBox = [this](const TCHAR* ComponentName, const FName& BoneName)
	{
		UBoxComponent* HitBox = CreateDefaultSubobject<UBoxComponent>(ComponentName);
		HitBox->SetupAttachment(GetMesh(), BoneName);
		HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HitCollisionBox.Add(BoneName, HitBox);
		return HitBox;
	};
	head = CreateHitBox(TEXT("head"), FName("head"));
	pelvis = CreateHitBox(TEXT("pelvis"), FName("pelvis"));
	spine_02 = CreateHitBox(TEXT("spine_02"), FName("spine_02"));
	spine_03 = CreateHitBox(TEXT("spine_03"), FName("spine_03"));
	upperArm_L = CreateHitBox(TEXT("upperarm_l"), FName("upperarm_l"));
	upperArm_R = CreateHitBox(TEXT("upperarm_r"), FName("upperarm_r"));
	lowerArm_L = CreateHitBox(TEXT("lowerarm_l"), FName("lowerarm_l"));
	lowerArm_R = CreateHitBox(TEXT("lowerarm_r"), FName("lowerarm_r"));
	hand_L = CreateHitBox(TEXT("hand_l"), FName("hand_l"));
	hand_R = CreateHitBox(TEXT("hand_r"), FName("hand_r"));
	backpack = CreateHitBox(TEXT("backpack"), FName("backpack"));
	blanket = CreateHitBox(TEXT("blanket"), FName("blanket"));
	thigh_L = CreateHitBox(TEXT("thigh_l"), FName("thigh_l"));
	thigh_R = CreateHitBox(TEXT("thigh_r"), FName("thigh_r"));
	calf_L = CreateHitBox(TEXT("calf_l"), FName("calf_l"));
	calf_R = CreateHitBox(TEXT("calf_r"), FName("calf_r"));
	foot_L = CreateHitBox(TEXT("foot_l"), FName("foot_l"));
	foot_R = CreateHitBox(TEXT("foot_r"), FName("foot_r"));
	for (auto& box :HitCollisionBox)
	{
		if (box.Value)
		{
			box.Value->SetCollisionObjectType(ECC_HitCollisionBox);
			box.Value->SetCollisionResponseToAllChannels(ECR_Ignore);
			box.Value->SetCollisionResponseToChannel(ECC_HitCollisionBox,ECR_Block);
		} 
	}
}

void AMyBlasterCharacter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	AttachHitBoxesToBones();
}


void AMyBlasterCharacter::SetTeamColor(ETeam Team)
{
	if (GetMesh() == nullptr || DefaultTeamM == nullptr) return;
	switch (Team)
	{
		case ETeam::ET_NoTeam:
			{
				GetMesh()->SetMaterial(0,DefaultTeamM);
				DissolveMaterialInstance = BlueDissolveMI;
				break;
			}
		case ETeam::ET_BlueTeam:
			{
				GetMesh()->SetMaterial(0,BlueTeamM);
				DissolveMaterialInstance = BlueDissolveMI;
				break;
			}
		case ETeam::ET_RedTeam:
			{
				GetMesh()->SetMaterial(0,RedTeamM);
				DissolveMaterialInstance = RedDissolveMI;
				break;
			}
	}
}

void AMyBlasterCharacter::BeginPlay()
{
	Super::BeginPlay();
	if (OverHeadWidget)
	{
		if (UOverHeadWidget* Widget = Cast<UOverHeadWidget>(OverHeadWidget->GetUserWidgetObject()))
		{
			Widget->ShowPlayerNetRole(this);
		}
	}
	UpdateHUDHealth();
	UpdateHUDShield();
	SpawnDefaultWeapon();
	if (HasAuthority())
	{
		OnTakeAnyDamage.AddDynamic(this, &AMyBlasterCharacter::ReceiveDamage);
	}
	if (AttachedGrenade)
	{
		AttachedGrenade->SetVisibility(false);
	}
}


void AMyBlasterCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	PlayerInputComponent->BindAxis("GoAhead", this, &AMyBlasterCharacter::MoveForward);
	PlayerInputComponent->BindAxis("MoveRight", this, &AMyBlasterCharacter::MoveRight);
	PlayerInputComponent->BindAxis("Yawn", this, &AMyBlasterCharacter::TurnAtRate);
	PlayerInputComponent->BindAxis("Pitch", this, &AMyBlasterCharacter::LookUpAtRate);

	PlayerInputComponent->BindAction("Jump", IE_Pressed, this, &AMyBlasterCharacter::Jump);
	PlayerInputComponent->BindAction("Equip", IE_Pressed, this, &AMyBlasterCharacter::EquipButtonPressed);
	PlayerInputComponent->BindAction("Crouch", IE_Pressed, this, &AMyBlasterCharacter::CrouchButtonPressed);
	PlayerInputComponent->BindAction("Aiming", IE_Pressed, this, &AMyBlasterCharacter::AimingButtonPressed);
	PlayerInputComponent->BindAction("Aiming", IE_Released, this, &AMyBlasterCharacter::AimingButtonReleased);
	PlayerInputComponent->BindAction("Reload", IE_Pressed, this, &AMyBlasterCharacter::Reload);

	PlayerInputComponent->BindAction("Fire", IE_Pressed, this, &AMyBlasterCharacter::FireButtonPressed);
	PlayerInputComponent->BindAction("Fire", IE_Released, this, &AMyBlasterCharacter::FireButtonReleased);
	PlayerInputComponent->BindAction("Throw", IE_Pressed, this, &AMyBlasterCharacter::ThrowButtonPressed);
}


void AMyBlasterCharacter::MoveForward(float AxisValue)
{
	if (bDisableGamePlay) return;
	if (Controller != nullptr && AxisValue != 0)
	{
		//
		//需要的是Controller的方向向量
		//
		const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FVector Direction(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X));
		AddMovementInput(Direction, AxisValue);
	}
}

void AMyBlasterCharacter::MoveRight(float AxisValue)
{
	if (bDisableGamePlay) return;
	if (Controller != nullptr && AxisValue != 0)
	{
		const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FVector Direction(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y));
		AddMovementInput(Direction, AxisValue);
	}
}

void AMyBlasterCharacter::TurnAtRate(float Rate)
{
	AddControllerYawInput(Rate);
}

void AMyBlasterCharacter::Reload()
{
	if (bDisableGamePlay) return;
	if (Combat)
	{
		Combat->Reload();
	}
}

void AMyBlasterCharacter::LookUpAtRate(float Rate)
{
	AddControllerPitchInput(Rate);
}

void AMyBlasterCharacter::CrouchButtonPressed()
{
	if (bDisableGamePlay) return;
	//Jump& Crouch是ACHaracter写好的，已经做了复制工作，后台完成 
	if (bIsCrouched)
	{
		UnCrouch();
	}
	else
	{
		Crouch();
	}
}

void AMyBlasterCharacter::AimingButtonPressed()
{
	if (bDisableGamePlay) return;
	if (Combat)
	{
		Combat->SetAiming(true);
	}
}

void AMyBlasterCharacter::AimingButtonReleased()

{
	if (bDisableGamePlay) return;
	if (Combat)
	{
		Combat->SetAiming(false);
	}
}

void AMyBlasterCharacter::ThrowButtonPressed()
{
	if (Combat)
	{
		Combat->ThrowGrenade();
	}
}

void AMyBlasterCharacter::Jump()
{
	if (bDisableGamePlay) return;
	if (bIsCrouched)
	{
		UnCrouch();
	}
	else
	{
		Super::Jump();
		if (JumpSound)
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				JumpSound,
				GetActorLocation()
			);
		}
	}
}

void AMyBlasterCharacter::FireButtonPressed()
{
	if (bDisableGamePlay) return;
	//关于武器的内容，都是Character做输入对接，然后个Combat做实现
	if (Combat)
	{
		Combat->FireButtonPressed(true);
	}
}

void AMyBlasterCharacter::FireButtonReleased()
{
	if (bDisableGamePlay) return;
	if (Combat)
	{
		Combat->FireButtonPressed(false);
	}
}

void AMyBlasterCharacter::ReceiveDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
	                                        class AController* InstigatorController, class AActor* DamageCauser)
{
	if (bElimmed) return;
	UWorld* World = GetWorld();
	if (World == nullptr) return;

	BlasterGameMode = BlasterGameMode == nullptr ?World->GetAuthGameMode<ABlasterGameMode>() : BlasterGameMode;
	if (BlasterGameMode == nullptr) return;
	float DamageToHealth  = BlasterGameMode->CalculateDamage(Damage,InstigatorController,Controller);
	if (Shield >0.f && DamageToHealth > 0.f)
	{
		const float DamageAbsorbedByShield = FMath::Min(Shield, DamageToHealth);
		Shield = FMath::Clamp(Shield - DamageAbsorbedByShield, 0.f, MaxShield);
		DamageToHealth = FMath::Clamp(DamageToHealth - DamageAbsorbedByShield, 0.f, DamageToHealth);
	}
	Health = FMath::Clamp(Health - DamageToHealth, 0, MaxHealth);
	//这样就可以改变health触发回调
	UpdateHUDHealth();
	UpdateHUDShield();
	//只有server，要考虑client
	PlayHitMontage();
	if (Health <= 0)
	{
		
		if (BlasterGameMode)
		{
			BlasterPlayerController = BlasterPlayerController == nullptr
				                          ? Cast<ABlasterPlayerController>(Controller)
				                          : BlasterPlayerController;
				ABlasterPlayerController* AttackerController = Cast<ABlasterPlayerController>(InstigatorController);
				BlasterGameMode->PlayerEliminated(this, BlasterPlayerController, AttackerController);
		}
	}
}

void AMyBlasterCharacter::UpdateHUDHealth()
{
	BlasterPlayerController = BlasterPlayerController == nullptr
		                          ? Cast<ABlasterPlayerController>(GetController())
		                          : BlasterPlayerController;
	if (BlasterPlayerController)
	{
		BlasterPlayerController->SetHealth(Health, MaxHealth);
	}
}

void AMyBlasterCharacter::UpdateHUDShield()
{
	BlasterPlayerController = BlasterPlayerController == nullptr
								  ? Cast<ABlasterPlayerController>(GetController())
								  : BlasterPlayerController;
	if (BlasterPlayerController)
	{
		BlasterPlayerController->SetShield(Shield, MaxShield);
	}
}

void AMyBlasterCharacter::MulticastGainLeader_Implementation()
{
	if (LeaderSystem == nullptr) return;
	if (!IsValid(LeaderSystemComponent))
	{
		// Codex: Attach the one-shot crown using relative space so it follows the character capsule.
		LeaderSystemComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			LeaderSystem,
			GetCapsuleComponent(),
			FName(),
			FVector(0.0f, 0.0f, 110.3f),
			FRotator::ZeroRotator,
			EAttachLocation::KeepRelativeOffset,
			false
			);
	}
	if (LeaderSystemComponent)
	{
		LeaderSystemComponent->Activate();
	}
}
void AMyBlasterCharacter::MulticastLostLeader_Implementation()
{
	if (LeaderSystemComponent)
	{
		//这里是destroy好吗？还是deact，可能存在太多会占内存？
		LeaderSystemComponent->DestroyComponent();
		// Codex: Clear the cached component pointer so the crown can be spawned again later.
		LeaderSystemComponent = nullptr;
	}
}

void AMyBlasterCharacter::UpdateAmmo()
{
	BlasterPlayerController = BlasterPlayerController == nullptr
								  ? Cast<ABlasterPlayerController>(GetController())
								  : BlasterPlayerController;
	if (BlasterPlayerController && Combat && Combat->EquippedWeapon)
	{
		BlasterPlayerController->SetCarriedAmmo(Combat->CarriedAmmo);
		BlasterPlayerController->SetAmmo(Combat->EquippedWeapon->GetAmmo());
	}
}

void AMyBlasterCharacter::OnRep_Health(float LastHealth)
{
	//Damage给server计算，然后这里health复制发生变化触发，这样就可以有动画了
	UpdateHUDHealth();
	//zai client update
	if (LastHealth > Health)
	{
		PlayHitMontage();
	}
}

void AMyBlasterCharacter::OnRep_Shield(float LastShield)
{
	UpdateHUDShield();
	//zai client update
	if (LastShield > Shield)
	{
		PlayHitMontage();
	}
}


void AMyBlasterCharacter::AimOffset(float DeltaSecond)
{
	if (Combat == nullptr || Combat->EquippedWeapon == nullptr)
	{
		StartingAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
		YawAO = 0.f;
		PitchAO = GetBaseAimRotation().Pitch;
		TurningInPlace = ETurningInPlace::ETIP_Noturning;
		return;
	}
	float Speed = CalculateSpeed();
	bool bIsInAir = GetCharacterMovement()->IsFalling();
	//判断其实可以给蓝图做，但是能在C++里就在这做完
	if (Speed <= 3.f && !bIsInAir)
	{
		bShouldRotateRootBone = true;
		FRotator CurrentRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
		//实际有反向问题，所以这里Starting放后面
		FRotator DeltaRotation = UKismetMathLibrary::NormalizedDeltaRotator(CurrentRotation, StartingAimRotation);
		YawAO = DeltaRotation.Yaw;
		if (TurningInPlace == ETurningInPlace::ETIP_Noturning)
		{
			InterpAOYaw = YawAO;
		}
		bUseControllerRotationYaw = true;
		TurnInPlace(DeltaSecond);
	}
	if (Speed > 3.f || bIsInAir) //移动过程中我们希望视角跟随controller，所以每帧记录当前方向。停下一瞬间，这个方向就是正对方向了。过程中OA为0
	{
		bShouldRotateRootBone = false;
		//这里Start一定要做帧处理
		StartingAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
		YawAO = 0;
		bUseControllerRotationYaw = true;
		TurningInPlace = ETurningInPlace::ETIP_Noturning;
	}
	//Pitch比较自用，你想跑的时候视角跟随，Aim的Yaw瞬间变换会很变扭。但是Pitch不一样，任何时候都可以变化
	//也支持低头，抬头初始开镜
	//这里有客户端传输问题。传输的角度非负，所以无法低头
	PitchAO = GetBaseAimRotation().Pitch;
	//所以单独处理client
	CalculatePitchAO();
}

void AMyBlasterCharacter::CalculatePitchAO()
{
	PitchAO = GetBaseAimRotation().Pitch;
	//所以单独处理client
	if (PitchAO > 90.f && !IsLocallyControlled())
	{
		//从270-360 变成-90 - 0映射
		FVector2D InRange(270.f, 360.f);
		FVector2D OutRange(-90.f, 0.f);
		PitchAO = FMath::GetMappedRangeValueClamped(InRange, OutRange, PitchAO);
	}
}

float AMyBlasterCharacter::CalculateSpeed()
{
	FVector Velocity = GetVelocity();
	Velocity.Z = 0.0f;
	return Velocity.Size();
}

void AMyBlasterCharacter::SimProxiesTurn()
{
	if (Combat == nullptr || Combat->EquippedWeapon == nullptr) return;
	bShouldRotateRootBone = false;
	float Speed = CalculateSpeed();

	if (Speed > 0.f)
	{
		TurningInPlace = ETurningInPlace::ETIP_Noturning;
		return;
	}

	LastProxyRotation = CurrentProxyRotation;
	CurrentProxyRotation = GetActorRotation();
	ProxyDeltaYaw = UKismetMathLibrary::NormalizedDeltaRotator(CurrentProxyRotation, LastProxyRotation).Yaw;
	if (FMath::Abs(ProxyDeltaYaw) > ProxyRotationThreshold)
	{
		if (ProxyDeltaYaw > ProxyRotationThreshold)
		{
			TurningInPlace = ETurningInPlace::ETIP_Right;
		}
		else
		{
			TurningInPlace = ETurningInPlace::ETIP_Left;
		}
		return;
	}
	TurningInPlace = ETurningInPlace::ETIP_Noturning;
}

void AMyBlasterCharacter::TurnInPlace(float DeltaSecond)
{
	if (YawAO > 90.f)
	{
		TurningInPlace = ETurningInPlace::ETIP_Right;
	}
	else if (YawAO < -90.f)
	{
		TurningInPlace = ETurningInPlace::ETIP_Left;
	}
	//
	//记得要在Aim状态外Noturning
	//
	if (TurningInPlace != ETurningInPlace::ETIP_Noturning)
	{
		//超出一定范围才开始做一个interp旋转
		InterpAOYaw = FMath::FInterpTo(InterpAOYaw, 0.f, DeltaSecond, 5.f);
		YawAO = InterpAOYaw;
		if (FMath::Abs(YawAO) < 15.f)
		{
			TurningInPlace = ETurningInPlace::ETIP_Noturning;
			StartingAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
		}
	}
}


void AMyBlasterCharacter::OnRep_OverlappingWeapon(AWeapon* LastWeapon)
{
	if (OverlappingWeapon)
	{
		OverlappingWeapon->ShowPickUpWidget(true);
	}
	//自动更新
	if (LastWeapon)
	{
		LastWeapon->ShowPickUpWidget(false);
	}
}

void AMyBlasterCharacter::PollInit()
{
	if (BlasterPlayerState == nullptr)
	{
		BlasterPlayerState = GetPlayerState<ABlasterPlayerState>();
		if (BlasterPlayerState)
		{
			BlasterPlayerState->AddToScore(0.f);
			BlasterPlayerState->AddToDefeat(0);
			SetTeamColor(BlasterPlayerState->GetCurrentTeam());
		}
		ABlasterGameState* GameState = Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
		if (GameState && GameState->TopScoringPlayers.Contains(BlasterPlayerState))
		{
			MulticastGainLeader();
		}
	}
}

void AMyBlasterCharacter::SpawnDefaultWeapon()
{
	BlasterGameMode = BlasterGameMode == nullptr ?GetWorld()->GetAuthGameMode<ABlasterGameMode>() : BlasterGameMode;
	UWorld* World = GetWorld();
	if (World && BlasterGameMode && DefaultWeaponClass)
	{
		AWeapon* StartingWeapon = World->SpawnActor<AWeapon>(DefaultWeaponClass);
		if (Combat && StartingWeapon)
		{
			StartingWeapon->bShouldDestroy=true;
			Combat->EquipWeapon(StartingWeapon);
		}
	}
}

void AMyBlasterCharacter::SetOverlappingWeapon(AWeapon* Weapon)
{
	//
	//这里的代码很重要，涉及到ListenService的显示问题，注意代码顺序
	// 
	//Attention这里的Weapon是更新前的
	if (OverlappingWeapon)
	{
		OverlappingWeapon->ShowPickUpWidget(false);
	}


	OverlappingWeapon = Weapon;
	//
	//为什么要写这一段？因为监听Server是不会进行OnRepOverlap的，所以看不到
	//
	if (IsLocallyControlled())
	{
		if (OverlappingWeapon)
		{
			OverlappingWeapon->ShowPickUpWidget(true);
		}
	}
}

void AMyBlasterCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	AttachHitBoxesToBones();
	if (Combat)
	{
		Combat->Character = this;
	}
	if (Buff)
	{
		Buff->Character = this;
		Buff->SetInitialSpeed(GetCharacterMovement()->MaxWalkSpeed,GetCharacterMovement()->MaxWalkSpeedCrouched);
		Buff->SetInitialJumpSpeed(GetCharacterMovement()->JumpZVelocity);
	}
	if (LagCompensation)
	{
		LagCompensation->Character = this;
		if (Controller)
		{
			LagCompensation->Controller=Cast<ABlasterPlayerController>(Controller);
		}
	}
}

void AMyBlasterCharacter::AttachHitBoxToBone(UBoxComponent* HitBox, const FName& BoneName)
{
	if (HitBox == nullptr || GetMesh() == nullptr) return;
	HitBox->AttachToComponent(GetMesh(), FAttachmentTransformRules::KeepRelativeTransform, BoneName);
}

void AMyBlasterCharacter::AttachHitBoxesToBones()
{
	AttachHitBoxToBone(head, FName("head"));
	AttachHitBoxToBone(pelvis, FName("pelvis"));
	AttachHitBoxToBone(spine_02, FName("spine_02"));
	AttachHitBoxToBone(spine_03, FName("spine_03"));
	AttachHitBoxToBone(upperArm_L, FName("upperarm_l"));
	AttachHitBoxToBone(upperArm_R, FName("upperarm_r"));
	AttachHitBoxToBone(lowerArm_L, FName("lowerarm_l"));
	AttachHitBoxToBone(lowerArm_R, FName("lowerarm_r"));
	AttachHitBoxToBone(hand_L, FName("hand_l"));
	AttachHitBoxToBone(hand_R, FName("hand_r"));
	AttachHitBoxToBone(backpack, FName("backpack"));
	AttachHitBoxToBone(blanket, FName("blanket"));
	AttachHitBoxToBone(thigh_L, FName("thigh_l"));
	AttachHitBoxToBone(thigh_R, FName("thigh_r"));
	AttachHitBoxToBone(calf_L, FName("calf_l"));
	AttachHitBoxToBone(calf_R, FName("calf_r"));
	AttachHitBoxToBone(foot_L, FName("foot_l"));
	AttachHitBoxToBone(foot_R, FName("foot_r"));
}

void AMyBlasterCharacter::PlayFireMontage(bool bIsAiming)
{
	if (Combat == nullptr || Combat->EquippedWeapon == nullptr) return;
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && FireWeaponMontage)
	{
		AnimInstance->Montage_Play(FireWeaponMontage);
		FName SectionName;
		SectionName = bIsAiming ? FName("RifleAim") : FName("RifleHip");
		AnimInstance->Montage_JumpToSection(SectionName, FireWeaponMontage);
	}
}

void AMyBlasterCharacter::PlayHitMontage()
{
	if (Combat == nullptr || Combat->EquippedWeapon == nullptr) return;
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && HitMontage)
	{
		AnimInstance->Montage_Play(HitMontage);
		FName SectionName("FromFront");
		AnimInstance->Montage_JumpToSection(SectionName, HitMontage);
	}
}

void AMyBlasterCharacter::PlayElimMontage()
{
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && ElimMontage)
	{
		AnimInstance->Montage_Play(ElimMontage);
	}
}

void AMyBlasterCharacter::PlayThrowMontage()
{
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && ThrowMontage)
	{
		AnimInstance->Montage_Play(ThrowMontage);
	}
}

void AMyBlasterCharacter::PlayReloadMontage()
{
	if (Combat == nullptr || Combat->EquippedWeapon == nullptr) return;
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && ReloadMontage)
	{
		AnimInstance->Montage_Play(ReloadMontage);
		FName SectionName;
		switch (Combat->EquippedWeapon->GetWeaponType())
		{
		case EWeaponType::EWT_AssaultRifle:
			{
				SectionName = FName("Rifle");
				break;
			}
		case EWeaponType::EWT_RocketLaucher:
			{
				SectionName = FName("RocketLauncher");
				break;
			}
		case EWeaponType::EWT_Pistol:
			{
				SectionName = FName("Pistol");
				break;
			}
		case EWeaponType::EWT_SubmachineGun:
			{
				SectionName = FName("Pistol");
				break;
			}
		case EWeaponType::EWT_ShotGun:
			{
				SectionName = FName("ShotGun");
				break;
			}
		case EWeaponType::EWT_SniperRifle:
			{
				SectionName = FName("SniperRifle");
				break;
			}
		case EWeaponType::EWT_GrenadeLaucher:
			{
				SectionName = FName("GrenadeLaucher");
				break;
			}
		}
		AnimInstance->Montage_JumpToSection(SectionName, ReloadMontage);
	}
}

void AMyBlasterCharacter::PlaySwapMontage()
{
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();;
	if (AnimInstance && SwapMontage)
	{
		AnimInstance->Montage_Play(SwapMontage );
	}
}


void AMyBlasterCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	HandleRotateInplace(DeltaTime);
	HideCameraIfClose();
	PollInit();
}

void AMyBlasterCharacter::HandleRotateInplace(float DeltaTime)
{
	if(bDisableGamePlay)
	{
		bUseControllerRotationYaw=false;;
		TurningInPlace=ETurningInPlace::ETIP_Noturning;
		return;
	}
	if (GetLocalRole() > ENetRole::ROLE_SimulatedProxy && IsLocallyControlled())
	{
		AimOffset(DeltaTime);
	}
	else
	{
		TimeSinceLastMovementRep += DeltaTime;
		if (TimeSinceLastMovementRep > 0.25f)
		{
			OnRep_ReplicatedMovement();
		}
		CalculatePitchAO();
	}
}


void AMyBlasterCharacter::OnRep_ReplicatedMovement()
{
	Super::OnRep_ReplicatedMovement();
	TimeSinceLastMovementRep = 0.f;
	SimProxiesTurn();
}

void AMyBlasterCharacter::Elim(bool bPlayerLeftGame)
{
	bLeftGame=bPlayerLeftGame;
	if (Combat)
	{
		TArray<AWeapon*> WeaponsToHandle;
		WeaponsToHandle.AddUnique(Combat->EquippedWeapon);
		WeaponsToHandle.AddUnique(Combat->PrimaryWeapon);
		WeaponsToHandle.AddUnique(Combat->SecondaryWeapon);

		for (AWeapon* Weapon : WeaponsToHandle)
		{
			if (Weapon == nullptr) continue;
			if (Weapon->bShouldDestroy)
			{
				Weapon->Destroy();
			}
			else
			{
				Weapon->Dropped();
			}
		}
	}
	MultiElim(bPlayerLeftGame);
}

void AMyBlasterCharacter::MultiElim_Implementation(bool bPlayerLeftGame)
{
	bLeftGame = bPlayerLeftGame;
	BlasterPlayerController = BlasterPlayerController == nullptr
		                          ? Cast<ABlasterPlayerController>(Controller)
		                          : BlasterPlayerController;
	if (BlasterPlayerController)
	{
		BlasterPlayerController->SetWeaponAmmoVisible(false);
	}
	bElimmed = true;
	PlayElimMontage();
	//Dissolve
	if (DissolveMaterialInstance)
	{
		DissolveMaterialInstanceDynamic = UMaterialInstanceDynamic::Create(DissolveMaterialInstance, this);

		GetMesh()->SetMaterial(0, DissolveMaterialInstanceDynamic);
		DissolveMaterialInstanceDynamic->SetScalarParameterValue(TEXT("Dissolve"), -0.44f);
		DissolveMaterialInstanceDynamic->SetScalarParameterValue(TEXT("Emissive"), 300.0f);
	}
	StartDissolve();
	
	//Disable character movement
	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->StopMovementImmediately();
	bDisableGamePlay = true;
	if (Combat)
	{
		Combat->FireButtonPressed(false);
	}
	//Disable Collision
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (AttachedGrenade)
	{
		AttachedGrenade->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	//Spawn Elim Bot
	if (ElimBotEffect)
	{
		FVector ElimBotPosition(GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z + 200.f);
		ElimBotComponent = UGameplayStatics::SpawnEmitterAtLocation(
			GetWorld(),
			ElimBotEffect,
			ElimBotPosition,
			GetActorRotation()
		);
	}
	if (ElimBotSound)
	{
		UGameplayStatics::SpawnSoundAtLocation(
			GetWorld(),
			ElimBotSound,
			GetActorLocation()
		);
	}
	bool bHideSniperScope = IsLocallyControlled()
	&&Combat
	&& Combat->EquippedWeapon
	&& Combat->bIsAiming
	&& Combat->EquippedWeapon->GetWeaponType() == EWeaponType::EWT_SniperRifle;
	if (bHideSniperScope)
	{
		ShowSniperScopeWidget(false);
	}
	if (LeaderSystemComponent)
	{
		LeaderSystemComponent->DestroyComponent();
		// Codex: Clear the cached component pointer during elimination cleanup.
		LeaderSystemComponent = nullptr;
	}
	
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ElimTimer,
			this,
			&AMyBlasterCharacter::ElimTimerFinished,
			ElimDelay);
	}
}

void AMyBlasterCharacter::ElimTimerFinished()
{
	UWorld* World = GetWorld();
	BlasterGameMode = BlasterGameMode == nullptr && World ?World->GetAuthGameMode<ABlasterGameMode>() : BlasterGameMode;
	if (BlasterGameMode && !bLeftGame)
	{
		BlasterGameMode->RequestRespawn(this, Controller);
	}
	if (bLeftGame && IsLocallyControlled())
	{
		OnLeftGame.Broadcast();
	}
}

void AMyBlasterCharacter::ServerLeaveGame_Implementation()
{
	UWorld* World = GetWorld();
	BlasterGameMode = BlasterGameMode == nullptr && World ?World->GetAuthGameMode<ABlasterGameMode>() : BlasterGameMode;
	BlasterPlayerState = BlasterPlayerState==nullptr? GetPlayerState<ABlasterPlayerState>() : BlasterPlayerState;
	if (BlasterGameMode && BlasterPlayerState)
	{
		BlasterGameMode->PlayerLeaveGame(BlasterPlayerState);
	}
}

void AMyBlasterCharacter::Destroyed()
{
	Super::Destroyed();
	if (ElimBotComponent)
	{
		ElimBotComponent->DestroyComponent();
	}
	//不加这个，死亡武器就会消失
	UWorld* World = GetWorld();
	BlasterGameMode = BlasterGameMode == nullptr && World ?World->GetAuthGameMode<ABlasterGameMode>() : BlasterGameMode;
	bool bMatchNotInProgress = BlasterGameMode && BlasterGameMode->GetMatchState() != MatchState::InProgress; 
	if (Combat && bMatchNotInProgress) 
	{
		TArray<AWeapon*> WeaponsToDestroy;
		WeaponsToDestroy.AddUnique(Combat->EquippedWeapon);
		WeaponsToDestroy.AddUnique(Combat->PrimaryWeapon);
		WeaponsToDestroy.AddUnique(Combat->SecondaryWeapon);

		for (AWeapon* Weapon : WeaponsToDestroy)
		{
			if (IsValid(Weapon))
			{
				Weapon->Destroy();
			}
		}
	}
	//因为每个Character触发这个都会有Destroy，所以在这里重载然后播报
}

void AMyBlasterCharacter::UpdateDissolveMaterial(float DissolveValue)
{
	if (DissolveMaterialInstanceDynamic)
	{
		DissolveMaterialInstanceDynamic->SetScalarParameterValue(TEXT("Dissolve"), DissolveValue);
	}
}

void AMyBlasterCharacter::StartDissolve()
{
	//Bind CallBack Function
	DissolveTrack.BindDynamic(this, &AMyBlasterCharacter::UpdateDissolveMaterial);
	if (DissolveCurve)
	{
		DissolveTimeline->AddInterpFloat(DissolveCurve, DissolveTrack);
		DissolveTimeline->Play();
	}
}

void AMyBlasterCharacter::HideCameraIfClose()
{
	if (!IsLocallyControlled()) return;
	if ((FollowCamera->GetComponentLocation() - GetActorLocation()).Size() < CameraThreshold)
	{
		GetMesh()->SetVisibility(false);
		if (Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponMesh())
		{
			Combat->EquippedWeapon->GetWeaponMesh()->bOwnerNoSee = true;
		}
		if (Combat && Combat->SecondaryWeapon && Combat->SecondaryWeapon->GetWeaponMesh())
		{
			Combat->SecondaryWeapon->GetWeaponMesh()->bOwnerNoSee = true;
		}
	}
	else
	{
		GetMesh()->SetVisibility(true);
		if (Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponMesh())
		{
			Combat->EquippedWeapon->GetWeaponMesh()->bOwnerNoSee = false;
		}
		if (Combat && Combat->SecondaryWeapon && Combat->SecondaryWeapon->GetWeaponMesh())
		{
			Combat->SecondaryWeapon->GetWeaponMesh()->bOwnerNoSee = false;
		}
	}
}

void AMyBlasterCharacter::ServerEquippedButtonPressed_Implementation()
{
	if (Combat)
	{
		if (OverlappingWeapon)
		{
			Combat->EquipWeapon(OverlappingWeapon);
		}
		else if (Combat->ShouldSwap())
		{
			Combat->SwapWeapon();
		}
	}
}

void AMyBlasterCharacter::EquipButtonPressed()
{
	if (Combat)
	{
		if (Combat->CombatState == ECombatState::ECS_Unoccupied)ServerEquippedButtonPressed();
		bool bShouldSwap = Combat->ShouldSwap() && OverlappingWeapon == nullptr  && !HasAuthority() && Combat->CombatState == ECombatState::ECS_Unoccupied;
		if (bShouldSwap)
		{
			PlaySwapMontage();
			Combat->CombatState = ECombatState::ECS_Swaping;
			bFinishSwapping = false;
		}
	}
}

bool AMyBlasterCharacter::IsWeaponEquipped()
{
	//根据有无Equip来确定
	return (Combat && Combat->EquippedWeapon);
}

bool AMyBlasterCharacter::IsAiming()
{
	return (Combat && Combat->bIsAiming);
}

AWeapon* AMyBlasterCharacter::GetEquippedWeapon()
{
	if (Combat == nullptr) return nullptr;
	return Combat->EquippedWeapon;
}

FVector AMyBlasterCharacter::HitTarget() const
{
	if (Combat == nullptr) return FVector();
	return Combat->HitTarget;
}

ECombatState AMyBlasterCharacter::GetCombatState() const
{
	if (Combat == nullptr) return ECombatState::ECS_MAX;
	return Combat->CombatState;
}

bool AMyBlasterCharacter::bIsLocallyReloading()
{
	if (Combat == nullptr) return false;
	return Combat->bLocallyReload;
}

void AMyBlasterCharacter::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	//这里是对所有CLient播报，所以我们需要Condition
	//DOREPLIFETIME(AMyBlasterCharacter, OverlappingWeapon);

	//DOREPLIFETIME是说明这个类中，这个CLass需要被复制。
	DOREPLIFETIME_CONDITION(AMyBlasterCharacter, OverlappingWeapon, COND_OwnerOnly);

	DOREPLIFETIME(AMyBlasterCharacter, Health);
	DOREPLIFETIME(AMyBlasterCharacter, bDisableGamePlay);
	DOREPLIFETIME(AMyBlasterCharacter, Shield);
}
