// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "../BlasterType/TurningInPlace.h"
#include "../BlasterType/CombatState.h"
#include "Blaster/BlasterType/Team.h"
#include "Sound/SoundBase.h"
#include "Blaster/Interfaces/InteractWithCrossHairInterface.h"
#include "Components/TimelineComponent.h"
#include "MyBlasterCharacter.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLeftGame);
UCLASS()
class BLASTER_API AMyBlasterCharacter : public ACharacter,public IInteractWithCrossHairInterface
{
	GENERATED_BODY()

public:
	AMyBlasterCharacter();
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	//Unreliable因为不重要
	
	void PlayFireMontage(bool bIsAiming);
	void PlayHitMontage();
	void PlayElimMontage();
	void PlayThrowMontage();
	void PlayReloadMontage();
	void PlaySwapMontage();
	virtual void OnRep_ReplicatedMovement() override;
	//serverOnly
	void Elim(bool bLeftGame);
	UFUNCTION(NetMulticast, reliable)
	void MultiElim(bool bLeftGame);
	virtual void Destroyed() override;

	UFUNCTION(BlueprintImplementableEvent)
	void ShowSniperScopeWidget(bool bShowScope);
	void UpdateHUDHealth();
	void UpdateHUDShield();

	bool bFinishSwapping = false;

	UFUNCTION(NetMulticast, Reliable)
	void MulticastGainLeader();
	
	UFUNCTION(NetMulticast, Reliable)
	void MulticastLostLeader();

	void SetTeamColor(ETeam Team);
protected:
	virtual void BeginPlay() override;
	void MoveForward(float AxisValue);
	void MoveRight(float AxisValue);
	void TurnAtRate(float Rate);
	void Reload();
	void SimProxiesTurn();
	void LookUpAtRate(float Rate);
	void EquipButtonPressed();
	void CrouchButtonPressed();
	//Fire
	void FireButtonPressed();
	void FireButtonReleased();
	//对于Trigged事件可以开按住和释放两个函数
	void AimingButtonPressed();
	void AimingButtonReleased();
	//Throw
	void ThrowButtonPressed();
	void AimOffset(float DeltaSecond);
	virtual void Jump() override;

	UFUNCTION()
	void ReceiveDamage(AActor* DamagedActor,float Damage,const UDamageType* DamageType,class AController* InstigatorController,class AActor* DamageCauser);

	void HandleRotateInplace(float DeltaTime);
	/*
	 *Hit boxes for client-server rewind
	 */
	UPROPERTY(EditAnywhere)
	class UBoxComponent* head;
		
	UPROPERTY(EditAnywhere)
	UBoxComponent* pelvis;

	UPROPERTY(EditAnywhere)
	UBoxComponent* spine_02;

	UPROPERTY(EditAnywhere)
	UBoxComponent* spine_03;

	UPROPERTY(EditAnywhere)
	UBoxComponent* upperArm_L;

	UPROPERTY(EditAnywhere)
	UBoxComponent* upperArm_R;
	
	UPROPERTY(EditAnywhere)
	UBoxComponent* lowerArm_L;
	
	UPROPERTY(EditAnywhere)
	UBoxComponent* lowerArm_R;
	
	UPROPERTY(EditAnywhere)
	UBoxComponent* hand_L;
	
	UPROPERTY(EditAnywhere)
	UBoxComponent* hand_R;

	UPROPERTY(EditAnywhere)
	UBoxComponent* backpack;

	UPROPERTY(EditAnywhere)
	UBoxComponent* blanket;

	UPROPERTY(EditAnywhere)
	UBoxComponent* thigh_L;

	UPROPERTY(EditAnywhere)
	UBoxComponent* thigh_R;

	UPROPERTY(EditAnywhere)
	UBoxComponent* calf_L;

	UPROPERTY(EditAnywhere)
	UBoxComponent* calf_R;

	UPROPERTY(EditAnywhere)
	UBoxComponent* foot_L;

	UPROPERTY(EditAnywhere)
	UBoxComponent* foot_R;

private:
	UPROPERTY(VisibleAnywhere,Category=Camera)
	class USpringArmComponent* CameraBoom;
	UPROPERTY(VisibleAnywhere,Category=Camera)
	class UCameraComponent* FollowCamera;
	UPROPERTY(EditAnywhere,BlueprintReadOnly,meta=(AllowPrivateAccess=true))
	class UWidgetComponent* OverHeadWidget;
	//可复制
	UPROPERTY(ReplicatedUsing=OnRep_OverlappingWeapon)
	class AWeapon* OverlappingWeapon;
	UFUNCTION()
	void OnRep_OverlappingWeapon(AWeapon* LastWeapon);
	//
	//流程 ：生成一个Weapon，绑定RepUsing到创建的反馈函数，然后在GetLifetimeReplicatedProps中声明这个Weapon是需要复制的（Weapon类要开可复制）
	//在Weapon类里设置Overlap反馈函数绑定在Overlap检测上，一旦Overlap，Overlap反馈函数讲this复制到指定Client，CLient的反馈函数一旦收到复制会激活，然后就可以显示了
	//
	
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,meta=(AllowPrivateAccess=true))
	class UCombatComponent* Combat;
	UPROPERTY(VisibleAnywhere)
	class UBuffComponent* Buff;
	UPROPERTY(VisibleAnywhere)
	class ULagCompensationComponent* LagCompensation;
	//
	//RPC
	//
	UFUNCTION(server, reliable)
	void ServerEquippedButtonPressed();

	float YawAO;
	float PitchAO;
	float InterpAOYaw;
	
	
	FRotator StartingAimRotation;
	ETurningInPlace TurningInPlace;
	void TurnInPlace(float DeltaSecond);
	void CalculatePitchAO();

	/*
	 *Montage Animation
	 */
	
	UPROPERTY(EditAnywhere,Category=Combat)
	class UAnimMontage* FireWeaponMontage;

	UPROPERTY(EditAnywhere,Category=Combat)
	UAnimMontage* HitMontage;

	UPROPERTY(EditAnywhere,Category=Combat)
	UAnimMontage* ElimMontage;

	UPROPERTY(EditAnywhere,Category=Combat)
	UAnimMontage* ReloadMontage;

	UPROPERTY(EditAnywhere,Category=Combat)
	UAnimMontage* ThrowMontage;

	UPROPERTY(EditAnywhere,Category=Combat)
	UAnimMontage* SwapMontage;
	
	void HideCameraIfClose();

	UPROPERTY(EditAnywhere)
	float CameraThreshold = 200.f;

	bool bShouldRotateRootBone;
	float ProxyRotationThreshold =0.5f;
	FRotator CurrentProxyRotation;
	FRotator LastProxyRotation;
	float ProxyDeltaYaw;
	float TimeSinceLastMovementRep;
	float CalculateSpeed();
	/*
	 *PlayerHealth
	 */
	UPROPERTY(EditAnywhere,Category = "Player Stats")
	float MaxHealth=100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Health,VisibleAnywhere,Category = "Player Stats")
	float Health = 100.f; 
	UFUNCTION()	
	void OnRep_Health(float LastHealth);

	/*
	 *PlayerShield
	 */
	UPROPERTY(EditAnywhere,Category = "Player Stats")
	float MaxShield=100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Shield,VisibleAnywhere,Category = "Player Stats")
	float Shield = 0.f; 
	UFUNCTION()	
	void OnRep_Shield(float LastShield);
	/*
	 *Ammo
	 */
	void UpdateAmmo();

	UPROPERTY()
	class ABlasterPlayerController* BlasterPlayerController;

	bool bElimmed=false;
	FTimerHandle ElimTimer;
	UPROPERTY(EditDefaultsOnly)
	float ElimDelay=3.f;
	void ElimTimerFinished();

	bool bLeftGame = false;
	
	
	/*
	 *Dissovle Effect
	 */
	UPROPERTY(VisibleAnywhere)
	UTimelineComponent* DissolveTimeline;

	UPROPERTY(EditAnywhere)
	UCurveFloat* DissolveCurve;
	
	FOnTimelineFloat DissolveTrack;
	
	UFUNCTION()
	void UpdateDissolveMaterial(float DissolveValue);
	
	void StartDissolve();

	//Dynamic Instance Changed At Runtime
	UPROPERTY(VisibleAnywhere,Category=ELim)
	UMaterialInstanceDynamic* DissolveMaterialInstanceDynamic;

	//Material instance set on the Blueprint ,used with dynamic material
	UPROPERTY(VisibleAnywhere,Category=ELim)
	UMaterialInstance* DissolveMaterialInstance;

	/*
	 *TeamColor
	 */
	UPROPERTY(EditAnywhere,Category=ELim)
	UMaterialInstance* RedDissolveMI;

	UPROPERTY(EditAnywhere,Category=ELim)
	UMaterialInstance* RedTeamM;
	
	UPROPERTY(EditAnywhere,Category=ELim)
	UMaterialInstance* BlueDissolveMI;
	
	UPROPERTY(EditAnywhere,Category=ELim)
	UMaterialInstance* BlueTeamM;

	UPROPERTY(EditAnywhere,Category=ELim)
	UMaterialInstance* DefaultTeamM;

	/*
	 *Elim Bot
	 */
	UPROPERTY(EditAnywhere,Category="Elim Bot")
	UParticleSystem* ElimBotEffect;

	UPROPERTY(VisibleAnywhere, Category="Elim Bot")
	UParticleSystemComponent* ElimBotComponent;

	UPROPERTY(EditAnywhere, Category="Elim Bot")
	class USoundCue* ElimBotSound;
	//初始化HUD信息
	UPROPERTY()
	class ABlasterPlayerState* BlasterPlayerState;
	void PollInit();
	/*
	 *Grenade
	 */
	UPROPERTY(VisibleAnywhere, Category="Grenade")
	UStaticMeshComponent* AttachedGrenade;
	/*
	 *Default Weapon
	 */
	UPROPERTY(EditAnywhere)
	TSubclassOf<AWeapon>DefaultWeaponClass;

	void SpawnDefaultWeapon();

	UPROPERTY(EditAnywhere)
	class UNiagaraSystem* LeaderSystem;

	UPROPERTY()
	class UNiagaraComponent* LeaderSystemComponent;
	
	UPROPERTY()
	class ABlasterGameMode* BlasterGameMode;
public:

	UFUNCTION(Server, Reliable)
	void ServerLeaveGame();

	FOnLeftGame OnLeftGame;
	
	//service端的Weapon重叠后，这个Weapon就会传入到这里，然后我们定义的OverlapWeapon就变成了this
	void SetOverlappingWeapon(AWeapon* Weapon);

	bool IsWeaponEquipped();
	bool IsAiming();
	FORCEINLINE float GetAOYaw() const{ return YawAO; }
	FORCEINLINE float GetPitchAO() const{ return PitchAO; }
	FORCEINLINE ETurningInPlace GetTurningInPlace() const{ return TurningInPlace; }
	FORCEINLINE UCameraComponent* GetFollowCamera() const{return FollowCamera;}
	FORCEINLINE bool ShouldRotateRootBone() const{ return bShouldRotateRootBone; }
	FORCEINLINE bool IsElimmed() const{ return bElimmed; }
	FORCEINLINE float GetHealth() const{ return Health; }
	FORCEINLINE float GetMaxHealth() const{ return MaxHealth; }
	FORCEINLINE UCombatComponent* GetCombat() const{ return Combat; }
	FORCEINLINE UAnimMontage* GetReloadMontage() const{ return ReloadMontage; }
	FORCEINLINE UStaticMeshComponent* GetAttachedGrenade() const{ return AttachedGrenade; }
	FORCEINLINE UBuffComponent* GetBuff() const{ return Buff; }
	FORCEINLINE void SetHealth(float NewHealth){ Health = NewHealth; }
	FORCEINLINE float GetShield() const { return Shield; }
	FORCEINLINE float GetMaxShield() const { return MaxShield; }
	FORCEINLINE void SetShield(float NewShield){ Shield = NewShield; }
	FORCEINLINE ULagCompensationComponent* GetLagCompensation() const{ return LagCompensation; }
	AWeapon* GetEquippedWeapon();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Audio")
	USoundBase* JumpSound;
	FVector HitTarget() const ;

	ECombatState GetCombatState() const;

	UPROPERTY(Replicated)
	bool bDisableGamePlay=false;

	bool bIsLocallyReloading();
	
	void AttachHitBoxToBone(UBoxComponent* HitBox, const FName& BoneName);
	void AttachHitBoxesToBones();

	UPROPERTY()
	TMap<FName,UBoxComponent*> HitCollisionBox;
	};
 
