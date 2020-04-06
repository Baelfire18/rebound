/**
 * @file    integrator_mercurana.c
 * @brief   MERCURANA is a symplectic multi-step method.
 *          It is adaptive, can handle close encounters and works in complex
 *          hierarchies.  
 * @author  Hanno Rein, Daniel Tamayo
 * 
 * @section LICENSE
 * Copyright (c) 2020 Hanno Rein, Daniel Tamayo
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "rebound.h"
#include "integrator.h"
#include "gravity.h"
#include "integrator_mercurana.h"
#include "integrator_eos.h"
#include "collision.h"
#define MIN(a, b) ((a) > (b) ? (b) : (a))    ///< Returns the minimum of a and b
#define MAX(a, b) ((a) > (b) ? (a) : (b))    ///< Returns the maximum of a and b

// Machine independent implementation of pow(*,1./3.) using Newton's method.
// Speed is not an issue. Only used to calculate dcrit.
static double sqrt3(double a){
    double x = 1.;
    for (int k=0; k<200;k++){  // A smaller number should be ok too.
        double x2 = x*x;
        x += (a/x2-x)/3.;
    }
    return x;
}

// Helper functions for L_infinity
static double f(double x){
    if (x<0) return 0;
    return exp(-1./x);
}

static double dfdy(double x){
    if (x<0) return 0;
    return exp(-1./x)/(x*x);
}

// Infinitely differentiable switching function.
static double reb_integrator_mercurana_L_infinity(const struct reb_simulation* const r, double d, double ri, double ro){
    double y = (d-ri)/(ro-ri);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 1.;
    }else{
        return f(y) /(f(y) + f(1.-y));
    }
}

// First derivative of the infinitely differentiable switching function.
static double reb_integrator_mercurana_dLdr_infinity(const struct reb_simulation* const r, double d, double ri, double ro){
    double y = (d-ri)/(ro-ri);
    double dydr = 1./(ro-ri);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 0.;
    }else{
        return dydr*(
                dfdy(y) /(f(y) + f(1.-y))
                -f(y) /(f(y) + f(1.-y))/(f(y) + f(1.-y)) * (dfdy(y) - dfdy(1.-y))
                );
    }
}

// This function returns the closest approach between particles p1 and p2 during a drift step with length dt
static double reb_mercurana_predict_rmin2(struct reb_particle p1, struct reb_particle p2, double dt){ 
    double dts = copysign(1.,dt); 
    dt = fabs(dt);
    const double dx1 = p1.x - p2.x; // distance at beginning
    const double dy1 = p1.y - p2.y;
    const double dz1 = p1.z - p2.z;
    const double r1 = (dx1*dx1 + dy1*dy1 + dz1*dz1);
    const double dvx1 = dts*(p1.vx - p2.vx); 
    const double dvy1 = dts*(p1.vy - p2.vy);
    const double dvz1 = dts*(p1.vz - p2.vz);
    const double dx2 = dx1 +dt*dvx1; // distance at end
    const double dy2 = dy1 +dt*dvy1;
    const double dz2 = dz1 +dt*dvz1;
    const double r2 = (dx2*dx2 + dy2*dy2 + dz2*dz2);
    const double t_closest = (dx1*dvx1 + dy1*dvy1 + dz1*dvz1)/(dvx1*dvx1 + dvy1*dvy1 + dvz1*dvz1);
    const double dx3 = dx1+t_closest*dvx1; // closest approach
    const double dy3 = dy1+t_closest*dvy1;
    const double dz3 = dz1+t_closest*dvz1;
    const double r3 = (dx3*dx3 + dy3*dy3 + dz3*dz3);

    double rmin2 = MIN(r1,r2);
    if (t_closest/dt>=0. && t_closest/dt<=1.){
        rmin2 = MIN(rmin2, r3);
    }
    return rmin2;
}

// This functions records a physical collision between particles i and j, to be resolved later.
static void reb_mercurana_record_collision(struct reb_simulation* const r, unsigned int i, unsigned int j){
    struct reb_simulation_integrator_mercurana* rim = &(r->ri_mercurana);
    if (r->collisions_allocatedN<=rim->collisions_N){
        // Allocate memory if there is no space in array.
        r->collisions_allocatedN = r->collisions_allocatedN ? r->collisions_allocatedN * 2 : 32;
        r->collisions = realloc(r->collisions,sizeof(struct reb_collision)*r->collisions_allocatedN);
    }
    r->collisions[rim->collisions_N].p1 = i;
    r->collisions[rim->collisions_N].p2 = j;
    struct reb_ghostbox gb = {0};
    gb.shiftx = r->particles[i].x;
    gb.shifty = r->particles[i].y;
    gb.shiftz = r->particles[i].z;
    gb.shiftvx = r->particles[i].vx;
    gb.shiftvy = r->particles[i].vy;
    gb.shiftvz = r->particles[i].vz;
    r->collisions[rim->collisions_N].gb = gb;
    rim->collisions_N++;
}

// This function checks if there are any close encounters or physical collisions between particles in a given shell during a drift step of length dt. If a close encounter occures, particles are placed in deeper shells.
static void reb_mercurana_encounter_predict(struct reb_simulation* const r, double dt, int shell){
    struct reb_simulation_integrator_mercurana* rim = &(r->ri_mercurana);
    struct reb_particle* const particles = r->particles;
    const double* const dcrit = rim->dcrit[shell];
    const int N = rim->shellN[shell];
    const int N_active = rim->shellN_active[shell];
    unsigned int* map = rim->map[shell];

    // Put all particles in current shell by default
    if (rim->N_dominant>0 && shell==1){
        for (int i=0; i<r->N; i++){ 
            // Set inshell to 1 for *all* particles
            // Needed to ensure all particles drift in shell 1 or deeper
            rim->inshell[i] = 1;
        }
    }else{
        for (int i=0; i<N; i++){
            int mi = map[i]; 
            rim->inshell[mi] = 1;
        }
    }
    
    if (shell+1>=rim->Nmaxshells){ // does sub-shell exist?
        return;
    }
	
    rim->collisions_N = 0;

    // Check if particles are in sub-shell
    rim->shellN[shell+1] = 0;
    rim->shellN_active[shell+1] = 0;

    // Note: Need to find the active particles first
    // TODO make this O(N^2/2)
    for (int i=0; i<N_active; i++){
        int mi = map[i]; 
        for (int j=0; j<N; j++){
            int mj = map[j]; 
            if (i==j) continue;
            double rmin2 = reb_mercurana_predict_rmin2(particles[mi],particles[mj],dt);
            double rsum = r->particles[mi].r+r->particles[mj].r;
            if (rmin2< rsum*rsum && r->collision==REB_COLLISION_DIRECT){
                reb_mercurana_record_collision(r,mi,mj);
            }
            double dcritsum = dcrit[mi]+dcrit[mj];
            if (rmin2< dcritsum*dcritsum){ 
                // j particle will be added later (active particles need to be in array first)
                rim->inshell[mi] = 0;
                rim->map[shell+1][rim->shellN[shell+1]] = mi;
                rim->shellN[shell+1]++;
                r->particles[mi].lastcollision = MAX(shell+1, r->particles[mi].lastcollision);
                r->particles[mj].lastcollision = MAX(shell+1, r->particles[mj].lastcollision);
                break; // only add particle i once
            }

        }
    }
    rim->shellN_active[shell+1] = rim->shellN[shell+1];
    for (int i=N_active; i<N; i++){
        int mi = map[i]; 
        for (int j=0; j<N_active; j++){
            int mj = map[j]; 
            double rmin2 = reb_mercurana_predict_rmin2(particles[mi],particles[mj],dt);
            double rsum = r->particles[mi].r+r->particles[mj].r;
            if (rmin2< rsum*rsum && r->collision==REB_COLLISION_DIRECT){
                reb_mercurana_record_collision(r,mi,mj);
            }
            double dcritsum = dcrit[mi]+dcrit[mj];
            if (rmin2< dcritsum*dcritsum){ 
                rim->inshell[mi] = 0;
                rim->map[shell+1][rim->shellN[shell+1]] = mi;
                rim->shellN[shell+1]++;
                r->particles[mi].lastcollision = MAX(shell+1, r->particles[mi].lastcollision);
                r->particles[mj].lastcollision = MAX(shell+1, r->particles[mj].lastcollision);
                break; // only add particle i once
            }
        }
    }
    if (rim->collisions_N){
        unsigned int N_before = r->N;
        reb_collision_search(r); // will resolve collisions
        if (N_before!=r->N){
            // Need to redo predict step as particles changed.
            reb_mercurana_encounter_predict(r, dt, shell);
        }
        rim->collisions_N = 0;
    }
}

// Main Kernel Operator: Interaction step. 
// y = timestep for acceleration
// v = timestep for jerk (0 if not used)
static void reb_integrator_mercurana_interaction_step(struct reb_simulation* r, double y, double v, unsigned int shell){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    struct reb_particle* const particles = r->particles;
    r->gravity = REB_GRAVITY_MERCURANA; // needed here again for SimulationArchive
    rim->current_shell = shell;
    reb_update_acceleration(r);
    if (v!=0.){
        reb_calculate_and_apply_jerk(r,v);
    }
    if (rim->N_dominant>0 && shell==1){
        // loop over *all* particles
        for (int i=0;i<r->N;i++){ // Apply acceleration. Jerk already applied.
            particles[i].vx += y*particles[i].ax;
            particles[i].vy += y*particles[i].ay;
            particles[i].vz += y*particles[i].az;
        }
    }else{
        const int N = rim->shellN[shell];
        unsigned int* map = rim->map[shell];
        for (int i=0;i<N;i++){ // Apply acceleration. Jerk already applied.
            const int mi = map[i];
            particles[mi].vx += y*particles[mi].ax;
            particles[mi].vy += y*particles[mi].ay;
            particles[mi].vz += y*particles[mi].az;
        }
    }
}

// Main Kernel Operator: Drift step. 
static void reb_integrator_mercurana_drift_step(struct reb_simulation* const r, double a, unsigned int shell){
#ifndef OPENMP
    if (reb_sigint) return;
#endif
    {
                //FILE* fp = fopen("/Users/rein/git/rebound/out.txt","a+");
                //fprintf(fp,"%.60f %.60f ", r->particles[1].x, r->particles[1].y);
                //fprintf(fp,"%.60f %.60f ", r->particles[1].vx, r->particles[1].vy);
                //fprintf(fp,"%.60f %.60f %d \n", r->t, a, shell);
                //fclose(fp);
    }
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    struct reb_particle* restrict const particles = r->particles;
    reb_mercurana_encounter_predict(r, a, shell);
    unsigned int* map = rim->map[shell];
    unsigned int N = rim->shellN[shell];
    if (shell==1 && rim->N_dominant>0){
        // Note: loop over *all* particles
        for (int i=0;i<r->N;i++){  
            // do not advance particles in subshells
            if(rim->inshell[i]){  
                particles[i].x += a*particles[i].vx;
                particles[i].y += a*particles[i].vy;
                particles[i].z += a*particles[i].vz;
            }
        }
    }
    // Note: no drift happens for shell==0 && N_dominant>0
    if (shell>1 || rim->N_dominant==0){
        for (int i=0;i<N;i++){  // loop over all particles in shell (includes subshells)
            int mi = map[i]; 
            // only advance in-shell particles
            if(rim->inshell[mi]){  
                particles[mi].x += a*particles[mi].vx;
                particles[mi].y += a*particles[mi].vy;
                particles[mi].z += a*particles[mi].vz;
            }
        }
    }
    if (shell+1<rim->Nmaxshells){ // does sub-shell exist?
        // Are there particles in it?
        // Is it a whstep?
        if (rim->shellN[shell+1]>0 || (shell==0 && rim->N_dominant>0)){
            rim->Nmaxshellsused = MAX(rim->Nmaxshellsused, shell+2);
            // advance all sub-shell particles
            unsigned int n = rim->n1?rim->n1:rim->n0;
            if (rim->n0>0 && shell==0){
                n = rim->n0; // use different number of substeps for first shell if WHsplitting is used
            }
            const double as = a/n;
            reb_integrator_eos_preprocessor(r, as, shell+1, rim->phi1, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);
            for (int i=0;i<n;i++){
                reb_integrator_eos_step(r, as, 1., 1., shell+1, rim->phi1, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);
            }
            reb_integrator_eos_postprocessor(r, as, shell+1, rim->phi1, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);
        }else{
            r->t += a;
        }
    }else{
        r->t += a;
    }
                //FILE* fp = fopen("/Users/rein/git/rebound/out.txt","a+");
                //fprintf(fp,"%.60f %.60f ", r->particles[1].x, r->particles[1].y);
                //fprintf(fp,"%.60f %.60f ", r->particles[1].vx, r->particles[1].vy);
                //fprintf(fp,"%.60f %.60f %d \n", r->t, a, shell);
                //fclose(fp);
}

// Part 1 only contains logic for setting up all the data structures. 
// The actual integration is done in part 2.
void reb_integrator_mercurana_part1(struct reb_simulation* r){
    if (r->var_config_N){
        reb_warning(r,"Mercurana does not work with variational equations.");
    }
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    if (rim->Nmaxshells<=0){
        reb_error(r,"Nmaxshells needs to be larger than 0.");
        return;
    }
    if (rim->Nmaxshells==1 && rim->n0 ){
        reb_error(r,"Nmaxshells>=2 is required if n0 is greater than 0.");
        return;
    }
    if (rim->Nmaxshells==2 && rim->n1){
        reb_error(r,"Nmaxshells>=3 is required if n1 is greater than 0.");
        return;
    }
    if (rim->Nmaxshells==1 && rim->N_dominant){
        reb_error(r,"Nmaxshells>=2 is required if N_dominant is used.");
        return;
    }
    if (rim->Nmaxshells>1 && rim->kappa<=0.){
        reb_error(r,"kappa>0 is required if Nmaxshells>1.");
        return;
    }
    
    const int N = r->N;
    
    if (rim->allocatedN<N){
        // dcrit
        if (rim->dcrit){
            for (int i=0;i<rim->Nmaxshells;i++){
                free(rim->dcrit[i]);
            }
        }
        rim->dcrit = realloc(rim->dcrit, sizeof(double*)*(rim->Nmaxshells));
        for (int i=0;i<rim->Nmaxshells;i++){
            rim->dcrit[i] = malloc(sizeof(double)*N);
        }
        // map
        if (rim->map){
            for (int i=0;i<rim->Nmaxshells;i++){
                free(rim->map[i]);
            }
        }
        rim->map = realloc(rim->map, sizeof(unsigned int*)*rim->Nmaxshells);
        for (int i=0;i<rim->Nmaxshells;i++){
            rim->map[i] = malloc(sizeof(unsigned int)*N);
        }
        for (int i=0;i<N;i++){
            // Set map to identity for outer-most shell
            rim->map[0][i] = i;
        }
        // inshell
        rim->inshell = realloc(rim->inshell, sizeof(unsigned int)*N);
        // shellN
        rim->shellN = realloc(rim->shellN, sizeof(unsigned int)*rim->Nmaxshells);
        // shellN_active
        rim->shellN_active = realloc(rim->shellN_active, sizeof(unsigned int)*rim->Nmaxshells);

        rim->allocatedN = N;
        // If particle number increased (or this is the first step), need to calculate critical radii
        rim->recalculate_dcrit_this_timestep = 1;
    }

    if (rim->recalculate_dcrit_this_timestep){
        rim->recalculate_dcrit_this_timestep = 0;
        if (rim->is_synchronized==0){
            reb_integrator_mercurana_synchronize(r);
            reb_warning(r,"MERCURANA: Recalculating dcrit but pos/vel were not synchronized before.");
        }
        double dt0 = r->dt;
        double dt_shell = r->dt;
        for (int s=0;s<rim->Nmaxshells;s++){ // innermost shell has no dcrit
            for (int i=0;i<N;i++){
                double mu = r->G*r->particles[i].m;
                double d0 = sqrt3(dt0*dt0/rim->kappa*mu/(4.*M_PI*M_PI));
                if (rim->alpha!=0.5){
                    // might not machine independent!
                    rim->dcrit[s][i] = pow(dt_shell/dt0,rim->alpha) * d0;
                }else{
                    rim->dcrit[s][i] = sqrt(dt_shell/dt0) * d0;
                }
            }
            // Definition: ??
            // - n=0 is not allowed
            // - n=1 means the D in a DKD scheme is calculated using one sub-step with DKD (0.5*dt)
            // - n=2 means the D in a DKD scheme is calculated using two DKD sub steps (0.25*dt each)
            // - n=0 is not allowed
            // - n=1 means the D in a DKDKD scheme is calculates using one sub-step of DKDKD (0.33*dt)
            // - n=2 means the D in a DKDKD scheme is calculated using two DKDKD sub-step (0.1666*dt each)
            double longest_drift_step_in_shell = 0.5; 
            enum REB_EOS_TYPE phi  = s==0?rim->phi0:rim->phi1;
            switch(phi){
                case REB_EOS_LF:
                case REB_EOS_PMLF4:
                    longest_drift_step_in_shell = 0.5; 
                    break;
                case REB_EOS_LF4:
                    longest_drift_step_in_shell = reb_eos_lf4_a;
                    break;
                case REB_EOS_LF6:
                    longest_drift_step_in_shell = reb_eos_lf6_a[0]+reb_eos_lf6_a[1];
                    break; 
                case REB_EOS_LF8: 
                    longest_drift_step_in_shell = reb_eos_lf8_a[0]+reb_eos_lf8_a[1];
                    break;
                case REB_EOS_LF4_2: 
                    longest_drift_step_in_shell = 1.-2.*reb_eos_lf4_2_a;
                    break;
                case REB_EOS_LF8_6_4:
                    longest_drift_step_in_shell = reb_eos_lf8_6_4_a[2];   
                case REB_EOS_PMLF6:
                    longest_drift_step_in_shell = reb_eos_pmlf6_a[1]; 
                    break;
                case REB_EOS_PLF7_6_4:
                    longest_drift_step_in_shell = reb_eos_plf7_6_4_a[0];   
                    break;
            }
            dt_shell *= longest_drift_step_in_shell;
            unsigned int n = rim->n0;
            if (s>0 && rim->n1){
                n = rim->n1; // use different number of substeps for deep shells
                             // if n1 is not set, keep using n0
            }
            dt_shell /= n;
            // Initialize shell numbers to zero (not needed, but helps debugging)
            rim->shellN[s] = 0;
            rim->shellN_active[s] = 0;
        }

    }
    
    // Calculate collisions only with DIRECT method
    if (r->collision != REB_COLLISION_NONE && r->collision != REB_COLLISION_DIRECT){
        reb_warning(r,"Mercurana only works with a direct collision search.");
    }
    
    // Calculate gravity with special function
    if (r->gravity != REB_GRAVITY_BASIC && r->gravity != REB_GRAVITY_MERCURANA){
        reb_warning(r,"Mercurana has it's own gravity routine. Gravity routine set by the user will be ignored.");
    }
    r->gravity = REB_GRAVITY_NONE; // Only temporary
    
    if (rim->L == NULL){
        // Setting default switching function
        rim->L = reb_integrator_mercurana_L_infinity;
        rim->dLdr = reb_integrator_mercurana_dLdr_infinity;
    }
}

// This routine performs one global timestep
void reb_integrator_mercurana_part2(struct reb_simulation* const r){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    if (rim->allocatedN<r->N){ // Error occured earlier.
        return;
    }
    rim->shellN[0] = r->N;
    rim->shellN_active[0] = r->N_active==-1?r->N:r->N_active;

    if (rim->is_synchronized){
        reb_integrator_eos_preprocessor(r, r->dt, 0, rim->phi0, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);
    }
    reb_integrator_eos_step(r, r->dt, 1., 1., 0, rim->phi0, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);

    rim->is_synchronized = 0;
    if (rim->safe_mode){
        reb_integrator_mercurana_synchronize(r);
    }

    //r->t+=r->dt;
    r->dt_last_done = r->dt;
}

// Apply post-processor to outermost splitting
void reb_integrator_mercurana_synchronize(struct reb_simulation* r){
    struct reb_simulation_integrator_mercurana* const rim = &(r->ri_mercurana);
    if (rim->is_synchronized == 0){
        if (rim->L == NULL){
            // Setting default switching function
            rim->L = reb_integrator_mercurana_L_infinity;
            rim->dLdr = reb_integrator_mercurana_dLdr_infinity;
        }
        reb_integrator_eos_postprocessor(r, r->dt, 0, rim->phi0, reb_integrator_mercurana_drift_step, reb_integrator_mercurana_interaction_step);
        rim->is_synchronized = 1;
    }
}

void reb_integrator_mercurana_reset(struct reb_simulation* r){
    if (r->ri_mercurana.allocatedN){
        for (int i=0;i<r->ri_mercurana.Nmaxshells;i++){
            free(r->ri_mercurana.map[i]);
            free(r->ri_mercurana.dcrit[i]);
        }
        free(r->ri_mercurana.map);
        free(r->ri_mercurana.dcrit);
        free(r->ri_mercurana.inshell);
        free(r->ri_mercurana.shellN);
        free(r->ri_mercurana.shellN_active);
    }
    r->ri_mercurana.allocatedN = 0;
    r->ri_mercurana.map = NULL;
    r->ri_mercurana.dcrit = NULL;
    r->ri_mercurana.inshell = NULL;
    r->ri_mercurana.shellN = NULL;
    r->ri_mercurana.shellN_active = NULL;
    
    r->ri_mercurana.phi0 = REB_EOS_LF;
    r->ri_mercurana.phi1 = REB_EOS_LF;
    r->ri_mercurana.n0 = 2;
    r->ri_mercurana.n1 = 0;
    r->ri_mercurana.kappa = 1e-3;
    r->ri_mercurana.alpha = 0.5;
    r->ri_mercurana.safe_mode = 1;
    r->ri_mercurana.Nmaxshells = 10;
    r->ri_mercurana.Nmaxshellsused = 1;
    r->ri_mercurana.recalculate_dcrit_this_timestep = 0;
    r->ri_mercurana.is_synchronized = 1;
    r->ri_mercurana.L = NULL;
    r->ri_mercurana.dLdr = NULL;
    r->ri_mercurana.collisions_N = 0;
    r->ri_mercurana.N_dominant = 0;
}

