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
import { EndpointListComponent } from './endpoint-list/endpoint-list.component';
import { MapComponent } from './map/map.component';
import { StatusBadgeComponent } from './common/status-badge/status-badge.component';
import { ToastComponent } from './common/toast/toast.component';
import { LogViewerComponent } from './log-level/log-viewer.component';

// Feature pages (from iot-ui)
import { VpnSubmenuComponent } from './vpn/vpn-submenu/vpn-submenu.component';
import { VpnConfigComponent } from './vpn/vpn-config/vpn-config.component';
import { VpnStatusComponent } from './vpn/vpn-status/vpn-status.component';
import { WanSubmenuComponent } from './wan/wan-submenu/wan-submenu.component';
import { WifiConfigComponent } from './wan/wifi-config/wifi-config.component';
import { WifiScanComponent } from './wan/wifi-scan/wifi-scan.component';
import { IfacePriorityComponent } from './wan/iface-priority/iface-priority.component';
import { RoutingSubmenuComponent } from './routing/routing-submenu/routing-submenu.component';
import { CustomRulesComponent } from './routing/custom-rules/custom-rules.component';
import { DeviceForwardingComponent } from './routing/device-forwarding/device-forwarding.component';
import { Lwm2mSubmenuComponent } from './lwm2m/lwm2m-submenu/lwm2m-submenu.component';
import { Lwm2mConfigComponent } from './lwm2m/lwm2m-config/lwm2m-config.component';
import { BsConfigComponent } from './lwm2m/bs-config/bs-config.component';
import { ServicesSubmenuComponent } from './services/services-submenu/services-submenu.component';
import { ServicesListComponent } from './services/services-list/services-list.component';
import { HttpConfigComponent } from './http-config/http-config.component';
import { UsersComponent } from './users/users.component';
import { SoftwareUpdateComponent } from './software-update/software-update.component';

import { SsoAuthInterceptor } from '../common/sso-auth.interceptor';
import { SessionService, initSession } from '../common/session.service';
import { DsHintComponent } from '../common/ds-hint.component';
import { DsDebugDirective } from '../common/ds-debug.directive';

@NgModule({
  declarations: [
    AppComponent, MainComponent, LoginComponent,
    DashboardComponent, EndpointListComponent, MapComponent, StatusBadgeComponent, ToastComponent, LogViewerComponent,
    VpnSubmenuComponent, VpnConfigComponent, VpnStatusComponent,
    WanSubmenuComponent, WifiConfigComponent, WifiScanComponent, IfacePriorityComponent,
    RoutingSubmenuComponent, CustomRulesComponent, DeviceForwardingComponent,
    Lwm2mSubmenuComponent, Lwm2mConfigComponent, BsConfigComponent,
    ServicesSubmenuComponent, ServicesListComponent, HttpConfigComponent,
    UsersComponent, SoftwareUpdateComponent,
    DsHintComponent, DsDebugDirective,
  ],
  imports: [
    BrowserModule, AppRoutingModule, BrowserAnimationsModule,
    ClarityModule, FormsModule, ReactiveFormsModule, HttpClientModule,
  ],
  providers: [
    { provide: HTTP_INTERCEPTORS, useClass: SsoAuthInterceptor, multi: true },
    { provide: APP_INITIALIZER, useFactory: initSession, deps: [SessionService], multi: true },
  ],
  bootstrap: [AppComponent]
})
export class AppModule { }
