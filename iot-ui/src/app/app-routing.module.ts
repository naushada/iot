import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';
import { LoginComponent } from './login/login.component';
import { MainComponent } from './main/main.component';
import { DashboardComponent } from './dashboard/dashboard.component';
import { SsoAuthGuard } from '../common/sso-auth.guard';

const routes: Routes = [
  { path: '', pathMatch: 'full', redirectTo: 'main' },
  { path: 'login', component: LoginComponent },
  {
    path: 'main', component: MainComponent,
    canActivate: [SsoAuthGuard],
    children: [
      { path: '', component: DashboardComponent },
      // Child routes for feature pages (Phase 3):
      // { path: 'vpn',       component: VpnConfigComponent },
      // { path: 'wan',       component: WanComponent },
      // { path: 'routing',   component: RoutingComponent },
      // { path: 'lwm2m',     component: Lwm2mComponent },
      // { path: 'services',  component: ServicesComponent },
    ]
  },
  { path: '**', redirectTo: 'main' }
];

@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule]
})
export class AppRoutingModule { }
