#include "Bullet.h"

#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"

ABullet::ABullet()
{
	PrimaryActorTick.bCanEverTick = false;
	StaticMesh=CreateDefaultSubobject<UStaticMeshComponent>("StaticMesh");
	SetRootComponent(StaticMesh);
	StaticMesh->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);
	StaticMesh->SetSimulatePhysics(true);
	StaticMesh->SetEnableGravity(true);
	StaticMesh->SetNotifyRigidBodyCollision(true);
	ShellEjectionImpulse=10.f;
}

void ABullet::BeginPlay()
{
	Super::BeginPlay();

	StaticMesh->AddImpulse(GetActorForwardVector() * ShellEjectionImpulse);

	StaticMesh->OnComponentHit.AddDynamic(this,&ABullet::OnHit);
}

void ABullet::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	if (bHasHit) return;
	bHasHit = true;

	if (ShellSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, ShellSound, GetActorLocation());
	}

	StaticMesh->SetNotifyRigidBodyCollision(false);
	StaticMesh->OnComponentHit.RemoveAll(this);

	SetLifeSpan(0.5f);
}
