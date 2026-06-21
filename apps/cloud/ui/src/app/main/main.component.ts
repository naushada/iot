import { Component, OnInit } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ThemeService } from '../../common/theme.service';
import { DebugService } from '../../common/debug.service';
import { DataStoreService } from '../../common/datastore.service';

@Component({
  selector: 'app-main',
  templateUrl: './main.component.html',
  styleUrls: ['./main.component.scss']
})
export class MainComponent implements OnInit {
  selectedMenu = 'dashboard';
  selectedSubNav = '';
  menus = [
    { id: 'dashboard', label: 'Dashboard', svg: 'assets/icons/dashboard.svg' },
    { id: 'endpoints', label: 'Endpoints', svg: 'assets/icons/endpoints.svg' },
    { id: 'map',       label: 'Map',       svg: 'assets/icons/map.svg' },
    { id: 'vpn',       label: 'VPN',       svg: 'assets/icons/vpn.svg' },
    { id: 'http',      label: 'HTTP',      svg: 'assets/icons/http.svg' },
    { id: 'routing',   label: 'Routing',   svg: 'assets/icons/routing.svg' },
    { id: 'lwm2m',     label: 'LwM2M',     svg: 'assets/icons/lwm2m.svg' },
    { id: 'services',  label: 'Services',  svg: 'assets/icons/services.svg' },
    { id: 'logs',      label: 'Logs',      svg: 'assets/icons/logs.svg' },
    { id: 'users',     label: 'Users',     svg: 'assets/icons/users.svg' },
    { id: 'software',  label: 'Software',  svg: 'assets/icons/software.svg' },
  ];

  version = '';   // running release (iot.version), shown in the sidebar footer

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private router: Router, public theme: ThemeService,
              public debug: DebugService,
              private ds: DataStoreService) { this.theme.init(); this.debug.init(); }

  ngOnInit(): void {
    // Authenticated shell: warm the shared data-store cache once so every
    // config page paints instantly, and start watching for live changes.
    this.ds.prefetchAll();
    this.ds.startWatch();
    // Running release version (written to ds by iot-httpd at startup).
    this.http.dbGet(['iot.version']).subscribe({
      next: (r) => {
        if (r.ok && r.data) this.version = String((r.data as Record<string, unknown>)['iot.version'] || '');
      }
    });
  }

  onMenuSelect(id: string): void { this.selectedMenu = id; this.selectedSubNav = ''; }

  /// Endpoints → "show on map": focus the Fleet Map on the clicked endpoint.
  mapFocus = '';
  goToMap(ep: string): void { this.mapFocus = ep; this.onMenuSelect('map'); }
  onSubNavSelect(item: string): void { this.selectedSubNav = item; }
  onLogout(): void { this.ds.stop(); this.http.logout().subscribe({ next: () => { this.session.clear(); this.router.navigate(['/login']); } }); }
}
