import { Component, OnInit } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ThemeService } from '../../common/theme.service';
import { DebugService } from '../../common/debug.service';
import { DataStoreService } from '../../common/datastore.service';
import { NavOrderService } from '../../common/nav-order.service';

@Component({
  selector: 'app-main',
  templateUrl: './main.component.html',
  styleUrls: ['./main.component.scss']
})
export class MainComponent implements OnInit {
  selectedMenu = 'dashboard';
  selectedSubNav = '';
  // Canonical order (the reset target). The displayed `menus` is reordered by
  // the per-browser saved order in ngOnInit and on drag.
  private readonly DEFAULT_MENUS = [
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
    { id: 'tenants',   label: 'Tenants',   svg: 'assets/icons/tenants.svg' },
    { id: 'audit',     label: 'Audit',     svg: 'assets/icons/audit.svg' },
    { id: 'software',  label: 'Software',  svg: 'assets/icons/software.svg' },
  ];
  menus = [...this.DEFAULT_MENUS];
  dragIndex = -1;   // index of the item being dragged (-1 = none)

  version = '';   // running release (iot.version), shown in the sidebar footer

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private router: Router, public theme: ThemeService,
              public debug: DebugService,
              private ds: DataStoreService,
              private navOrder: NavOrderService) { this.theme.init(); this.debug.init(); }

  ngOnInit(): void {
    // Apply the per-browser saved sidebar order (new pages append).
    this.menus = this.navOrder.apply([...this.DEFAULT_MENUS]);
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

  // ── Drag-to-reorder the sidebar (HTML5 DnD; persisted per browser) ──
  onDragStart(i: number): void { this.dragIndex = i; }
  onDragOver(e: DragEvent, i: number): void {
    e.preventDefault();                       // allow the drop
    if (this.dragIndex < 0 || this.dragIndex === i) return;
    const [moved] = this.menus.splice(this.dragIndex, 1);
    this.menus.splice(i, 0, moved);           // live reorder as you hover
    this.dragIndex = i;
  }
  onDragEnd(): void {
    if (this.dragIndex < 0) return;
    this.dragIndex = -1;
    this.navOrder.save(this.menus.map((m) => m.id));
  }
  resetNav(): void {
    this.navOrder.reset();
    this.menus = [...this.DEFAULT_MENUS];
  }

  /// Endpoints → "show on map": focus the Fleet Map on the clicked endpoint.
  mapFocus = '';
  goToMap(ep: string): void { this.mapFocus = ep; this.onMenuSelect('map'); }
  onSubNavSelect(item: string): void { this.selectedSubNav = item; }
  onLogout(): void { this.ds.stop(); this.http.logout().subscribe({ next: () => { this.session.clear(); this.router.navigate(['/login']); } }); }
}
