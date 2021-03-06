// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeometryCollection/GeometryCollectionCollisionStructureManager.h"
#include "ChaosLog.h"
#include "Chaos/Box.h"
#include "Chaos/ErrorReporter.h"
#include "Chaos/Levelset.h"
#include "Chaos/Particles.h"
#include "Chaos/Sphere.h"
#include "Chaos/Vector.h"
#include "HAL/IConsoleManager.h"
#include "Chaos/TriangleMesh.h"
#include "Chaos/PBDRigidClustering.h"

DEFINE_LOG_CATEGORY_STATIC(GCS_Log, NoLogging, All);

FCollisionStructureManager::FCollisionStructureManager()
{
}

int32 bCollisionParticlesUseImplicitCulling = false;
FAutoConsoleVariableRef CVarCollisionParticlesUseImplicitCulling(TEXT("p.CollisionParticlesUseImplicitCulling"), bCollisionParticlesUseImplicitCulling, TEXT("Use the implicit to cull interior vertices."));

int32 CollisionParticlesSpatialDivision = 10;
FAutoConsoleVariableRef CVarCollisionParticlesSpatialDivision(TEXT("p.CollisionParticlesSpatialDivision"), CollisionParticlesSpatialDivision, TEXT("Spatial bucketing to cull collision particles."));

int32 CollisionParticlesMin = 10;
FAutoConsoleVariableRef CVarCollisionParticlesMin(TEXT("p.CollisionParticlesMin"), CollisionParticlesMin, TEXT("Minimum number of particles after simplicial pruning (assuming it started with more)"));

int32 CollisionParticlesMax = 2000;
FAutoConsoleVariableRef CVarCollisionParticlesMax(TEXT("p.CollisionParticlesMax"), CollisionParticlesMax, TEXT("Maximum number of particles after simplicial pruning"));

FCollisionStructureManager::FSimplicial*
FCollisionStructureManager::NewSimplicial(
	const Chaos::TParticles<float, 3>& Vertices,
	const Chaos::TTriangleMesh<float>& TriMesh,
	const Chaos::FImplicitObject* Implicit,
	const int32 CollisionParticlesMaxInput)
{
	FCollisionStructureManager::FSimplicial * Simplicial = new FCollisionStructureManager::FSimplicial();
	if (Implicit || Vertices.Size())
	{
		float Extent = 0;
		int32 LSVCounter = 0;
		TSet<int32> Indices;
		TriMesh.GetVertexSet(Indices);
		TArray<Chaos::TVector<float, 3>> OutsideVertices;

		bool bFullCopy = true;
		int32 LocalCollisionParticlesMax = CollisionParticlesMaxInput > 0 ? FMath::Min(CollisionParticlesMaxInput, CollisionParticlesMax) : CollisionParticlesMax;
		if (bCollisionParticlesUseImplicitCulling!=0 && Implicit && Indices.Num()>LocalCollisionParticlesMax)
		{
			Extent = Implicit->HasBoundingBox() ? Implicit->BoundingBox().Extents().Size() : 1.f;

			float Threshold = Extent * 0.01;

			//
			//  Remove particles inside the levelset. (I think this is useless) 
			//
			OutsideVertices.AddUninitialized(Indices.Num());
			for (int32 Idx : Indices)
			{
				const Chaos::TVector<float, 3>& SamplePoint = Vertices.X(Idx);
				if (Implicit->SignedDistance(SamplePoint) > Threshold)
				{
					OutsideVertices[LSVCounter] = SamplePoint;
					LSVCounter++;
				}
			}
			OutsideVertices.SetNum(LSVCounter);

			if (OutsideVertices.Num() > LocalCollisionParticlesMax)
				bFullCopy = false;
		}
		
		if(bFullCopy)
		{
			FBox Bounds(ForceInitToZero);
			TArray<int32> IndicesArray = Indices.Array();
			OutsideVertices.AddUninitialized(Indices.Num());
			for (int32 Idx=0;Idx<IndicesArray.Num();Idx++)
			{
				Bounds += FVector(Vertices.X(IndicesArray[Idx]));
				OutsideVertices[Idx] = Vertices.X(IndicesArray[Idx]);
			}
			Extent = Bounds.GetExtent().Size();
		}

		//
		// Clean the particles based on distance
		//

		float SnapThreshold = Extent / float(CollisionParticlesSpatialDivision);
		OutsideVertices = Chaos::CleanCollisionParticles(OutsideVertices, SnapThreshold);
		int32 NumParticles = (OutsideVertices.Num() > LocalCollisionParticlesMax) ? LocalCollisionParticlesMax : OutsideVertices.Num();

		if (NumParticles)
		{
			int32 VertexCounter = 0;
			Simplicial->AddParticles(NumParticles);
			for (int32 i = 0; i<NumParticles;i++)
			{
				if (!OutsideVertices[i].ContainsNaN())
				{
					Simplicial->X(i) = OutsideVertices[i];
					VertexCounter++;
				}
			}
			Simplicial->Resize(VertexCounter);
		}

		if(!Simplicial->Size())
		{
			Simplicial->AddParticles(1);
			Simplicial->X(0) = Chaos::TVector<float, 3>(0);
		}

		Simplicial->UpdateAccelerationStructures();

		UE_LOG(LogChaos, Log, TEXT("NewSimplicial: InitialSize: %d, ImplicitExterior: %d, FullCopy: %d, FinalSize: %d"), Indices.Num(), LSVCounter, (int32)bFullCopy, NumParticles);
		return Simplicial;
	}
	UE_LOG(LogChaos, Log, TEXT("NewSimplicial::Empty"));
	return Simplicial;
}

FCollisionStructureManager::FSimplicial*
FCollisionStructureManager::NewSimplicial(
	const Chaos::TParticles<float,3>& AllParticles,
	const TManagedArray<int32>& BoneMap,
	const ECollisionTypeEnum CollisionType,
	Chaos::TTriangleMesh<float>& TriMesh,
	const float CollisionParticlesFraction)
{
	const bool bEnableCollisionParticles = (CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric);
	if (bEnableCollisionParticles || true)
	{
		// @todo : Clean collision particles need to operate on the collision mask from the DynamicCollection,
		//         then transfer only the good collision particles during the initialization. `
		FCollisionStructureManager::FSimplicial * Simplicial = new FCollisionStructureManager::FSimplicial();
		const TArrayView<const Chaos::TVector<float, 3>> ArrayView(&AllParticles.X(0), AllParticles.Size());
		const TArray<Chaos::TVector<float,3>>& Result = Chaos::CleanCollisionParticles(TriMesh, ArrayView, CollisionParticlesFraction);

		if (Result.Num())
		{
			int32 VertexCounter = 0;
			Simplicial->AddParticles(Result.Num());
			for (int Index = Result.Num() - 1; 0 <= Index; Index--)
			{
				if (!Result[Index].ContainsNaN())
				{
					Simplicial->X(Index) = Result[Index];
					VertexCounter++;
				}
			}
			Simplicial->Resize(VertexCounter);
		}

		if (!Simplicial->Size())
		{
			Simplicial->AddParticles(1);
			Simplicial->X(0) = Chaos::TVector<float, 3>(0);
		}

		Simplicial->UpdateAccelerationStructures();

		return Simplicial;
	}
	return new FCollisionStructureManager::FSimplicial();
}

void FCollisionStructureManager::UpdateImplicitFlags(
	FImplicit* Implicit, 
	const ECollisionTypeEnum CollisionType)
{
	if (ensure(Implicit))
	{
		Implicit->SetCollisionType(CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric ? Chaos::ImplicitObjectType::LevelSet : Implicit->GetType());
		if (Implicit && (CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric) && Implicit->GetType() == Chaos::ImplicitObjectType::LevelSet)
		{
			Implicit->SetDoCollide(false);
			Implicit->SetConvex(false);
		}
	}
}

FCollisionStructureManager::FImplicit* 
FCollisionStructureManager::NewImplicit(
	Chaos::FErrorReporter ErrorReporter,
	const Chaos::TParticles<float, 3>& MeshParticles,
	const Chaos::TTriangleMesh<float>& TriMesh,
	const FBox& CollisionBounds,
	const float Radius, 
	const int32 MinRes,
	const int32 MaxRes,
	const float CollisionObjectReduction,
	const ECollisionTypeEnum CollisionType,
	const EImplicitTypeEnum ImplicitType)
{
	if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box)
	{
		return NewImplicitBox(CollisionBounds, CollisionObjectReduction, CollisionType);
	}
	else if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere)
	{
		return NewImplicitSphere(Radius, CollisionObjectReduction, CollisionType);
	}
	else if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_LevelSet)
	{
		return NewImplicitLevelset(ErrorReporter, MeshParticles, TriMesh, CollisionBounds, MinRes, MaxRes, CollisionObjectReduction, CollisionType);
	}
	
	return nullptr;
}

FCollisionStructureManager::FImplicit*
FCollisionStructureManager::NewImplicitBox(
	const FBox& CollisionBounds,
	const float CollisionObjectReduction,
	const ECollisionTypeEnum CollisionType)
{
	const FVector HalfExtents = CollisionBounds.GetExtent() * (1 - CollisionObjectReduction / 100.f);
	const FVector Center = CollisionBounds.GetCenter();
	Chaos::FImplicitObject* Implicit = new Chaos::TBox<float, 3>(Center - HalfExtents, Center + HalfExtents);
	UpdateImplicitFlags(Implicit, CollisionType);
	return Implicit;
}

FCollisionStructureManager::FImplicit*
FCollisionStructureManager::NewImplicitSphere(
	const float Radius,
	const float CollisionObjectReduction,
	const ECollisionTypeEnum CollisionType)
{
	Chaos::FImplicitObject* Implicit = new Chaos::TSphere<float, 3>(Chaos::TVector<float, 3>(0), Radius * (1 - CollisionObjectReduction / 100.f));
	UpdateImplicitFlags(Implicit, CollisionType);
	return Implicit;
}

FCollisionStructureManager::FImplicit*
FCollisionStructureManager::NewImplicitLevelset(
	Chaos::FErrorReporter ErrorReporter,
	const Chaos::TParticles<float, 3>& MeshParticles,
	const Chaos::TTriangleMesh<float>& TriMesh,
	const FBox& CollisionBounds,
	const int32 MinRes,
	const int32 MaxRes,
	const float CollisionObjectReduction,
	const ECollisionTypeEnum CollisionType)
{
	FVector HalfExtents = CollisionBounds.GetExtent();
	if (HalfExtents.GetAbsMin() < KINDA_SMALL_NUMBER)
	{
		return nullptr;
	}
	Chaos::TLevelSet<float, 3>* LevelSet = NewLevelset(ErrorReporter, MeshParticles, TriMesh, CollisionBounds, MinRes, MaxRes, CollisionType);
	if (LevelSet)
	{
		const float DomainVolume = LevelSet->BoundingBox().GetVolume();
		const float FilledVolume = LevelSet->ApproximateNegativeMaterial();
		if (FilledVolume < DomainVolume * 0.05)
		{
			const Chaos::TVector<float,3> Extent = LevelSet->BoundingBox().Extents();
			ErrorReporter.ReportLog(
				*FString::Printf(TEXT(
					"Level set is small or empty:\n"
					"    domain extent: (%g %g %g) volume: %g\n"
					"    estimated level set volume: %g\n"
					"    percentage filled: %g%%"),
					Extent[0], Extent[1], Extent[2], DomainVolume, FilledVolume, FilledVolume / DomainVolume * 100.0));
		}
		HalfExtents *= CollisionObjectReduction / 100.f;
		const float MinExtent = FMath::Min(HalfExtents[0], FMath::Min(HalfExtents[1], HalfExtents[2]));
		if (MinExtent > 0)
		{
			LevelSet->Shrink(MinExtent);
		}
	}
	else
	{
		ErrorReporter.ReportError(TEXT("Level set rasterization failed."));
	}
	return LevelSet;
}

Chaos::TLevelSet<float, 3>* FCollisionStructureManager::NewLevelset(
	Chaos::FErrorReporter ErrorReporter,
	const Chaos::TParticles<float, 3>& MeshParticles,
	const Chaos::TTriangleMesh<float>& TriMesh,
	const FBox& CollisionBounds,
	const int32 MinRes,
	const int32 MaxRes,
	const ECollisionTypeEnum CollisionType)
{
	if(TriMesh.GetNumElements() == 0)
	{
		// Empty tri-mesh, can't create a new level set
		return nullptr;
	}

	Chaos::TVector<int32, 3> Counts;
	const FVector Extents = CollisionBounds.GetExtent();
	if (Extents.X < Extents.Y && Extents.X < Extents.Z)
	{
		Counts.X = MinRes;
		Counts.Y = MinRes * static_cast<int32>(Extents.Y / Extents.X);
		Counts.Z = MinRes * static_cast<int32>(Extents.Z / Extents.X);
	}
	else if (Extents.Y < Extents.Z)
	{
		Counts.X = MinRes * static_cast<int32>(Extents.X / Extents.Y);
		Counts.Y = MinRes;
		Counts.Z = MinRes * static_cast<int32>(Extents.Z / Extents.Y);
	}
	else
	{
		Counts.X = MinRes * static_cast<int32>(Extents.X / Extents.Z);
		Counts.Y = MinRes * static_cast<int32>(Extents.Y / Extents.Z);
		Counts.Z = MinRes;
	}
	if (Counts.X > MaxRes)
	{
		Counts.X = MaxRes;
	}
	if (Counts.Y > MaxRes)
	{
		Counts.Y = MaxRes;
	}
	if (Counts.Z > MaxRes)
	{
		Counts.Z = MaxRes;
	}
	Chaos::TUniformGrid<float, 3> Grid(CollisionBounds.Min, CollisionBounds.Max, Counts, 1);
	Chaos::TLevelSet<float, 3>* Implicit = new Chaos::TLevelSet<float, 3>(ErrorReporter, Grid, MeshParticles, TriMesh);
	if (ErrorReporter.ContainsUnhandledError())
	{
		ErrorReporter.HandleLatestError();	//Allow future levelsets to attempt to cook
		delete Implicit;
		return nullptr;
	}
	UpdateImplicitFlags(Implicit, CollisionType);
	return Implicit;
}

FVector 
FCollisionStructureManager::CalculateUnitMassInertiaTensor(
	const FBox& Bounds,
	const float Radius,
	const EImplicitTypeEnum ImplicitType
)
{	
	FVector Tensor(1);
	if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box)
	{
		const Chaos::TVector<float, 3> Size = Bounds.GetSize();
		const Chaos::PMatrix<float, 3, 3> I = Chaos::TAABB<float, 3>::GetInertiaTensor(1.0, Size);
		Tensor = { I.M[0][0], I.M[1][1], I.M[2][2] };
	}
	else if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere)
	{
		Tensor = FVector(Chaos::TSphere<float, 3>::GetInertiaTensor(1.0, Radius, false).M[0][0]);
	}
	ensureMsgf(Tensor.X != 0.f && Tensor.Y != 0.f && Tensor.Z != 0.f, TEXT("Rigid bounds check failure."));
	return Tensor;
}

float
FCollisionStructureManager::CalculateVolume(
	const FBox& Bounds,
	const float Radius,
	const EImplicitTypeEnum ImplicitType
)
{
	float Volume = 1.f;
	if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box)
	{
		Volume = Bounds.GetVolume();
	}
	else if (ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere)
	{
		Volume = Chaos::TSphere<float, 3>::GetVolume(Radius);
	}
	ensureMsgf(Volume != 0.f, TEXT("Rigid volume check failure."));
	return Volume;
}
