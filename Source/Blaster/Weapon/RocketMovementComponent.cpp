#include "RocketMovementComponent.h"

UProjectileMovementComponent::EHandleBlockingHitResult URocketMovementComponent::HandleBlockingHit(
	const FHitResult& Hit, float TimeTick, const FVector& MoveDelta, float& SubTickTimeRemaining)
{
	Super::HandleBlockingHit(Hit, TimeTick, MoveDelta, SubTickTimeRemaining);
	return EHandleBlockingHitResult::AdvanceNextSubstep;
}


    
void URocketMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	//Nothing
	//Rockets should not stop only explode when their collision box detect a hit  
}
