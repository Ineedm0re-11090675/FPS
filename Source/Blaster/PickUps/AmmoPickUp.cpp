#include "AmmoPickUp.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"

void AAmmoPickUp::OnSphereOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
                                  UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	Super::OnSphereOverlap(OverlappedComponent, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);

	AMyBlasterCharacter* MyCharacter = Cast<AMyBlasterCharacter>(OtherActor);
	if (MyCharacter == nullptr) return;

	UCombatComponent* Combat = MyCharacter->GetCombat();
	if (Combat == nullptr) return;

	Combat->PickUpAmmo(WeaponType, AmmoAmount);
	Destroy();
}
