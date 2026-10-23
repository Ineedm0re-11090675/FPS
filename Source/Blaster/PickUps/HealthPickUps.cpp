#include "HealthPickUps.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/BuffComponent/BuffComponent.h"
AHealthPickups::AHealthPickups()
{
	// Codex: Health pickup uses Niagara only; hide the placeholder mesh that shows WorldGrid gray.
	SetPickupMeshVisible(false);
	bReplicates = true;
}

bool AHealthPickups::ShouldHidePickupMesh() const
{
	return true;
}

void AHealthPickups::OnSphereOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
                                     UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	Super::OnSphereOverlap(OverlappedComponent, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);

	AMyBlasterCharacter* MyCharacter = Cast<AMyBlasterCharacter>(OtherActor);
	if (MyCharacter)
	{
		UBuffComponent* BuffComponent = MyCharacter->GetBuff();
		if (BuffComponent)
		{
			BuffComponent->Heal(HealthAmount,HealthTime);
		}
	}
	Destroy();
}
