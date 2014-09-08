/// \file
/// Leapfrog time integrator

#include "timestep.h"

#include "CoMDTypes.h"
#include "linkCells.h"
#include "neighborList.h"
#include "hashTable.h"
#include "parallel.h"
#include "defines.h"
#include "gpu_neighborList.h"
#include "performanceTimers.h"

#include <stdio.h>


void redistributeAtomsGpu(SimFlat* sim);
void redistributeAtomsGpuNL(SimFlat* sim);
void redistributeAtomsCpuNL(SimFlat* sim);

void advanceVelocity(SimFlat* sim, real_t dt);
void advancePosition(SimFlat* sim, real_t dt);

// computes local potential and kinetic energies
EXTERN_C void computeEnergy(SimFlat *sim, real_t *eLocal);	

EXTERN_C void advanceVelocityGpu(SimGpu sim, real_t dt);
static   void advanceVelocityCpu(SimFlat* sim, int nBoxes, real_t dt);
EXTERN_C void advancePositionGpu(SimGpu* sim, real_t dt);
static   void advancePositionCpu(SimFlat* sim, int nBoxes, real_t dt);

EXTERN_C int compactCells(char* compactAtoms, char* work_d, int nCells, int *d_cellList, SimGpu sim_gpu, int* d_cellOffsets, int * d_workScan, real3_old shift);

EXTERN_C void getAtomMsgSoAPtr(char* const buffer, AtomMsgSoA *atomMsg, int n);
EXTERN_C void buildAtomListGpu(SimFlat *sim, cudaStream_t stream);
EXTERN_C void updateLinkCellsGpu(SimFlat *sim);
EXTERN_C void sortAtomsGpu(SimFlat *sim, cudaStream_t stream);

EXTERN_C int neighborListUpdateRequiredGpu(SimGpu* sim);
EXTERN_C void updateNeighborsGpuAsync(SimGpu sim, int *temp, int num_cells, int *cell_list, cudaStream_t stream);

EXTERN_C void eamForce1GpuAsync(SimGpu sim, AtomListGpu atoms_list, int num_cells, int *cells_list, int method, cudaStream_t stream);
EXTERN_C void eamForce2GpuAsync(SimGpu sim, AtomListGpu atoms_list, int num_cells, int *cells_list, int method, cudaStream_t stream);

EXTERN_C void emptyHashTableGpu(HashTableGpu* hashTable);
EXTERN_C void neighborListForceRebuildGpu(struct NeighborListGpuSt* neighborList); 

/// Advance the simulation time to t+dt using a leap frog method
/// (equivalent to velocity verlet).
///
/// Forces must be computed before calling the integrator the first time.
///
///  - Advance velocities half time step using forces
///  - Advance positions full time step using velocities
///  - Update link cells and exchange remote particles
///  - Compute forces
///  - Update velocities half time step using forces
///
/// This leaves positions, velocities, and forces at t+dt, with the
/// forces ready to perform the half step velocity update at the top of
/// the next call.
///
/// After nSteps the kinetic energy is computed for diagnostic output.
double timestep(SimFlat* s, int nSteps, real_t dt)
{
   for (int ii=0; ii<nSteps; ++ii)
   {
      startTimer(velocityTimer);
      advanceVelocity(s, 0.5*dt); 
      stopTimer(velocityTimer);

      startTimer(positionTimer);
      advancePosition(s, dt); 
      stopTimer(positionTimer);

#if 0 //TODO async version
      if(s->method == 3 || s->method == 4){
              if(s->gpuAsync)
                      buildNeighborList(s, INTERIOR);  // sim->interior_stream
      }
#endif

      //TODO overlap redistribute with buildNeighborListInterior()
      startTimer(redistributeTimer);
      redistributeAtoms(s);
      stopTimer(redistributeTimer);

      //TODO overlap buildNeighborListBoundary with computeForce()
      if(s->method == THREAD_ATOM_NL || s->method == WARP_ATOM_NL || s->method == CPU_NL){
         startTimer(neighborListBuildTimer);
#if 0 //TODO async version
         if(s->gpuAsync)
                 buildNeighborList(s, BOUNDARY); // sim->boundary_stream
         else
#endif
                 buildNeighborList(s, 0); 
         stopTimer(neighborListBuildTimer);
      }

      startTimer(computeForceTimer);
      computeForce(s);
      stopTimer(computeForceTimer);

      startTimer(velocityTimer);
      advanceVelocity(s, 0.5*dt); 
      stopTimer(velocityTimer);
   }

   if(s->method < CPU_NL)
      kineticEnergyGpu(s);
   else
      kineticEnergy(s);

   return s->ePotential;
}

void computeForce(SimFlat* s)
{
   s->pot->force(s);
}

/// Calculates total kinetic and potential energy across all tasks.  The
/// local potential energy is a by-product of the force routine.
void kineticEnergy(SimFlat* s)
{
   real_t eLocal[2];
   eLocal[0] = s->ePotential;
   eLocal[1] = 0;
   for (int iBox=0; iBox<s->boxes->nLocalBoxes; iBox++)
   {
      for (int iOff=MAXATOMS*iBox,ii=0; ii<s->boxes->nAtoms[iBox]; ii++,iOff++)
      {
         int iSpecies = s->atoms->iSpecies[iOff];
         real_t invMass = 0.5/s->species[iSpecies].mass;
         eLocal[1] += ( s->atoms->p.x[iOff] * s->atoms->p.x[iOff] +
         s->atoms->p.y[iOff] * s->atoms->p.y[iOff] +
         s->atoms->p.z[iOff] * s->atoms->p.z[iOff] )*invMass;
      }
   }

   real_t eSum[2];
   startTimer(commReduceTimer);
   addRealParallel(eLocal, eSum, 2);
   stopTimer(commReduceTimer);

   s->ePotential = eSum[0];
   s->eKinetic = eSum[1];
}

void advanceVelocity(SimFlat* s, real_t dt)
{
      if(s->method < CPU_NL)
              advanceVelocityGpu(s->gpu, dt); 
      else
              advanceVelocityCpu(s, s->boxes->nLocalBoxes, dt); 
}

void advanceVelocityCpu(SimFlat* s, int nBoxes, real_t dt)
{
   for (int iBox=0; iBox<nBoxes; iBox++)
   {
      for (int iOff=MAXATOMS*iBox,ii=0; ii<s->boxes->nAtoms[iBox]; ii++,iOff++)
      {
         s->atoms->p.x[iOff] += dt*s->atoms->f.x[iOff];
         s->atoms->p.y[iOff] += dt*s->atoms->f.y[iOff];
         s->atoms->p.z[iOff] += dt*s->atoms->f.z[iOff];
      }
   }
}

void advancePosition(SimFlat* s, real_t dt)
{
      if(s->method < CPU_NL)
              advancePositionGpu(&(s->gpu), dt); 
      else
              advancePositionCpu(s, s->boxes->nLocalBoxes, dt);
}

void advancePositionCpu(SimFlat* s, int nBoxes, real_t dt)
{
   for (int iBox=0; iBox<nBoxes; iBox++)
   {
      for (int iOff=MAXATOMS*iBox,ii=0; ii<s->boxes->nAtoms[iBox]; ii++,iOff++)
      {
         int iSpecies = s->atoms->iSpecies[iOff];
         real_t invMass = 1.0/s->species[iSpecies].mass;
         s->atoms->r.x[iOff] += dt*s->atoms->p.x[iOff]*invMass;
         s->atoms->r.y[iOff] += dt*s->atoms->p.y[iOff]*invMass;
         s->atoms->r.z[iOff] += dt*s->atoms->p.z[iOff]*invMass;
      }
   }
   if(s->method == CPU_NL)
//next call to neighborListUpdateRequired() will loop over all particles TODO: this functionality should not be here (see advanceGPU() for further comments)
      s->atoms->neighborList->updateNeighborListRequired = -1; 
}

/// Calculates total kinetic and potential energy across all tasks.  The
/// local potential energy is a by-product of the force routine.
void kineticEnergyGpu(SimFlat* s)
{
   real_t eLocal[2];

   computeEnergy(s, eLocal);

   real_t eSum[2];
   startTimer(commReduceTimer);
   addRealParallel(eLocal, eSum, 2);
   stopTimer(commReduceTimer);

   s->ePotential = eSum[0];
   s->eKinetic = eSum[1];
}

/// \details
/// This function provides one-stop shopping for the sequence of events
/// that must occur for a proper exchange of halo atoms after the atom
/// positions have been updated by the integrator.
///
/// - updateLinkCells: Since atoms have moved, some may be in the wrong
///   link cells.
/// - haloExchange (atom version): Sends atom data to remote tasks. 
/// - sort: Sort the atoms.
///
/// \see updateLinkCells
/// \see initAtomHaloExchange
/// \see sortAtomsInCell
void redistributeAtoms(SimFlat* sim)
{ 
   if(sim->method == THREAD_ATOM_NL || sim->method == WARP_ATOM_NL) 
      redistributeAtomsGpuNL(sim);
   else if(sim->method == CPU_NL) 
      redistributeAtomsCpuNL(sim);
   else
      redistributeAtomsGpu(sim);
}

void redistributeAtomsGpu(SimFlat* sim)
{ 
   updateLinkCellsGpu(sim);

   // cell lists are updated 
   // now we can launch force computations on the interior
   if (sim->gpuAsync) {
     // only update neighbors list when method != 0
     if (sim->method != THREAD_ATOM) 
       updateNeighborsGpuAsync(sim->gpu, sim->flags, sim->gpu.boxes.nLocalBoxes - sim->n_boundary_cells, sim->interior_cells, sim->interior_stream);

     int n_interior_cells = sim->gpu.boxes.nLocalBoxes - sim->n_boundary_cells;
     eamForce1GpuAsync(sim->gpu, sim->gpu.i_list, n_interior_cells, sim->interior_cells, sim->method, sim->interior_stream);
     eamForce2GpuAsync(sim->gpu, sim->gpu.i_list, n_interior_cells, sim->interior_cells, sim->method, sim->interior_stream);
   }

   // exchange is only for boundaries
   startTimer(atomHaloTimer);
   haloExchange(sim->atomExchange, sim);
   stopTimer(atomHaloTimer);

   buildAtomListGpu(sim, sim->boundary_stream);

   // sort only boundary cells
   sortAtomsGpu(sim, sim->boundary_stream);
}

void redistributeAtomsGpuNL(SimFlat* sim)
{
   int nlUpdateRequired = neighborListUpdateRequired(sim);

   emptyHaloCellsGpu(sim);

   //If nlUpdateRequired is set, we will rebuild the hashtable during this call to haloExchange.
   //Hence, we have to clear it first.
   if(nlUpdateRequired){

      emptyHashTableGpu(&(sim->gpu.d_hashTable));
      updateLinkCellsGpu(sim);
   }

   // cell lists are updated 
   // now we can launch force computations on the interior
   if (sim->gpuAsync) {
     int n_interior_cells = sim->gpu.boxes.nLocalBoxes - sim->n_boundary_cells;
     eamForce1GpuAsync(sim->gpu, sim->gpu.i_list, n_interior_cells, sim->interior_cells, sim->method, sim->interior_stream);
     eamForce2GpuAsync(sim->gpu, sim->gpu.i_list, n_interior_cells, sim->interior_cells, sim->method, sim->interior_stream);
   }

   sim->gpu.d_hashTable.nEntriesGet = 0;

   startTimer(atomHaloTimer);
   haloExchange(sim->atomExchange, sim);
   stopTimer(atomHaloTimer);

   int haloExchangeRequired=0;
   cudaMemcpy(&haloExchangeRequired, sim->gpu.d_updateLinkCellsRequired,sizeof(int), cudaMemcpyDeviceToHost); //TODO Async? boundary_stream
   int flag = 0; //indicates if any particle has migrated from one processor to the other 
   //sync boundary stream TODO
   addIntParallel(&haloExchangeRequired, &flag, 1); 

   //this is required if one particle has moved from one processor to the other (required by forceExchange)
   //right now we force all processors to rebuild their NL because the haloExchange would deadlock otherwise.
   //However, this situation can be avoided with some additional logic within the haloExchange() function.
   if(flag > 0){
           neighborListForceRebuildGpu(&(sim->gpu.atoms.neighborList));

           emptyHaloCellsGpu(sim);
           emptyHashTableGpu(&(sim->gpu.d_hashTable));

           sim->gpu.d_hashTable.nEntriesGet = 0; //REFACTORING TODO move into haloexchange()? (this has to be set everytime we call haloExchange && method == 3)

           startTimer(atomHaloTimer);
           haloExchange(sim->atomExchange, sim);
           stopTimer(atomHaloTimer);

           cudaMemset(sim->gpu.d_updateLinkCellsRequired,0,sizeof(int));
   }

   buildAtomListGpu(sim, sim->boundary_stream);
}

void redistributeAtomsCpuNL(SimFlat* sim)
{
   int nlUpdateRequired = neighborListUpdateRequired(sim);

   emptyHaloCells(sim->boxes);

   //If nlUpdateRequired is set, we will rebuild the hashtable during this call to haloExchange.
   //Hence, we have to clear it first.
   if(nlUpdateRequired){
      emptyHashTable(sim->atomExchange->hashTable);
      updateLinkCellsCpu(sim->boxes, sim->atoms);
   }
   
   sim->atomExchange->hashTable->nEntriesGet = 0;

   startTimer(atomHaloTimer);
   haloExchange(sim->atomExchange, sim);
   stopTimer(atomHaloTimer);


   int flag = 0; //indicates if any processor has received some particle from its neighbor
   addIntParallel(&(sim->atoms->neighborList->updateLinkCellsRequired), &flag, 1);

   //this is required if one particle has moved from one processor to the other (required by forceExchange)
   //right now we force all processors to rebuild their NL because the haloExchange would deadlock otherwise.
   //However, this situation can be avoided with some additional logic within the haloExchange() function.
   if(flag > 0){
           neighborListForceRebuild(sim->atoms->neighborList); 

           atomsUpdateLocalId(sim->boxes, sim->atoms);
           emptyHaloCells(sim->boxes);

           //if nlUpdateRequired is set, we will rebuild the hashtable during this call to haloExchange
           //Hence, we have to clear it first.
           emptyHashTable(sim->atomExchange->hashTable);

           sim->atomExchange->hashTable->nEntriesGet = 0;

           startTimer(atomHaloTimer);
           haloExchange(sim->atomExchange, sim);
           stopTimer(atomHaloTimer);

           sim->atoms->neighborList->updateLinkCellsRequired = 0;
   }

   if(nlUpdateRequired)
      atomsUpdateLocalId(sim->boxes, sim->atoms);
                
}
