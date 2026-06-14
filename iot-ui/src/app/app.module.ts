import { APP_INITIALIZER, NgModule } from '@angular/core';
import { BrowserModule } from '@angular/platform-browser';
import { HTTP_INTERCEPTORS, HttpClientModule } from '@angular/common/http';
import { BrowserAnimationsModule } from '@angular/platform-browser/animations';
import { ClarityModule } from '@clr/angular';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';

import { AppRoutingModule } from './app-routing.module';
import { AppComponent } from './app.component';
import { MainComponent } from './main/main.component';
import { LoginComponent } from './login/login.component';
import { DashboardComponent } from './dashboard/dashboard.component';
import { StatusBadgeComponent } from './common/status-badge/status-badge.component';

import { VpnSubmenuComponent } from './vpn/vpn-submenu/vpn-submenu.component';
import { VpnConfigComponent } from './vpn/vpn-config/vpn-config.component';
import { VpnStatusComponent } from './vpn/vpn-status/vpn-status.component';

import { WanSubmenuComponent } from './wan/wan-submenu/wan-submenu.component';
import { WifiConfigComponent } from './wan/wifi-config/wifi-config.component';
import { WifiScanComponent } from './wan/wifi-scan/wifi-scan.component';
import { IfacePriorityComponent } from './wan/iface-priority/iface-priority.component';

import { RoutingSubmenuComponent } from './routing/routing-submenu/routing-submenu.component';
import { PortForwardComponent } from './routing/port-forward/port-forward.component';
import { CustomRulesComponent } from './routing/custom-rules/custom-rules.component';

import { Lwm2mSubmenuComponent } from './lwm2m/lwm2m-submenu/lwm2m-submenu.component';
import { Lwm2mConfigComponent } from './lwm2m/lwm2m-config/lwm2m-config.component';

import { ServicesSubmenuComponent } from './services/services-submenu/services-submenu.component';
import { ServicesListComponent } from './services/services-list/services-list.component';
import { LogLevelComponent } from './log-level/log-level.component';
import { LogViewerComponent } from './log-level/log-viewer.component';
import { ToastComponent } from './common/toast/toast.component';
import { UsersComponent } from './users/users.component';
import { SoftwareUpdateComponent } from './software-update/software-update.component';
import { HttpConfigComponent } from './http-config/http-config.component';

import { SsoAuthInterceptor } from '../common/sso-auth.interceptor';
import { SessionService, initSession } from '../common/session.service';
import { DsHintComponent } from '../common/ds-hint.component';
import { DsDebugDirective } from '../common/ds-debug.directive';

@NgModule({
  declarations: [
    AppComponent,
    MainComponent,
    LoginComponent,
    DashboardComponent,
    StatusBadgeComponent,
    VpnSubmenuComponent, VpnConfigComponent, VpnStatusComponent,
    WanSubmenuComponent, WifiConfigComponent, WifiScanComponent, IfacePriorityComponent,
    RoutingSubmenuComponent, PortForwardComponent, CustomRulesComponent,
    Lwm2mSubmenuComponent, Lwm2mConfigComponent,
    ServicesSubmenuComponent, ServicesListComponent,
    LogLevelComponent, LogViewerComponent, ToastComponent,
    UsersComponent, SoftwareUpdateComponent, HttpConfigComponent,
    DsHintComponent, DsDebugDirective,
  ],
  imports: [
    BrowserModule,
    AppRoutingModule,
    BrowserAnimationsModule,
    ClarityModule,
    FormsModule,
    ReactiveFormsModule,
    HttpClientModule,
  ],
  providers: [
    { provide: HTTP_INTERCEPTORS, useClass: SsoAuthInterceptor, multi: true },
    { provide: APP_INITIALIZER, useFactory: initSession,
      deps: [SessionService], multi: true },
  ],
  bootstrap: [AppComponent]
})
export class AppModule { }
