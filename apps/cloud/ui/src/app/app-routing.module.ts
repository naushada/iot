import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';
import { LoginComponent } from './login/login.component';
import { MainComponent } from './main/main.component';
import { DashboardComponent } from './dashboard/dashboard.component';
import { EndpointListComponent } from './endpoint-list/endpoint-list.component';
import { SsoAuthGuard } from '../common/sso-auth.guard';

const routes: Routes = [
  { path: '', pathMatch: 'full', redirectTo: 'main' },
  { path: 'login', component: LoginComponent },
  { path: 'main', component: MainComponent, canActivate: [SsoAuthGuard],
    children: [
      { path: '', component: DashboardComponent },
      { path: 'endpoints', component: EndpointListComponent },
    ]
  },
  { path: '**', redirectTo: 'main' }
];

@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule]
})
export class AppRoutingModule { }
