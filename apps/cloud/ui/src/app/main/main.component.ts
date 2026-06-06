import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ThemeService } from '../../common/theme.service';

@Component({
  selector: 'app-main',
  templateUrl: './main.component.html',
  styleUrls: ['./main.component.scss']
})
export class MainComponent {
  selectedMenu = 'dashboard';
  selectedSubNav = '';
  menus = [
    { id: 'dashboard', label: 'Dashboard', svg: 'assets/icons/dashboard.svg' },
    { id: 'endpoints', label: 'Endpoints', svg: 'assets/icons/routing.svg' },
    { id: 'vpn',       label: 'VPN',       svg: 'assets/icons/vpn.svg' },
    { id: 'wan',       label: 'WAN',       svg: 'assets/icons/wan.svg' },
    { id: 'routing',   label: 'Routing',   svg: 'assets/icons/routing.svg' },
    { id: 'lwm2m',     label: 'LwM2M',     svg: 'assets/icons/lwm2m.svg' },
    { id: 'services',  label: 'Services',  svg: 'assets/icons/services.svg' },
    { id: 'logs',      label: 'Logs',      svg: 'assets/icons/logs.svg' },
  ];

  constructor(private http: HttpsvcService, private session: SessionService,
              private router: Router, public theme: ThemeService) { this.theme.init(); }

  onMenuSelect(id: string): void { this.selectedMenu = id; this.selectedSubNav = ''; }
  onSubNavSelect(item: string): void { this.selectedSubNav = item; }
  onLogout(): void { this.http.logout().subscribe({ next: () => { this.session.clear(); this.router.navigate(['/login']); } }); }
}
