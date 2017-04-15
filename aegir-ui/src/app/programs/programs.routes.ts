import { Routes } from '@angular/router';

import { AddProgramComponent } from './add-program.component';
import { ProgramComponent } from './program.component';

export const PROGRAMS_ROUTES: Routes = [
    { path: '', redirectTo: 'add', pathMatch: 'full' },
    { path: 'add', component: AddProgramComponent },
    { path: ':id/view', component: ProgramComponent }
]
