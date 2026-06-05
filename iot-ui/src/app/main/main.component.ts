import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { PubSubService } from '../../common/pubsubsvc.service';
import { StatusSnapshot } from '../../common/app-globals';

@Component({
  selector: 'app-main',
  templateUrl: './main.component.html',
  styleUrls: ['./main.component.scss']
})
export class MainComponent {
  selectedMenu = 'dashboard';
  selectedSubNav = '';      // submenu item label
  selectedSubItem = '';     // event from submenu
  today = new Date().toLocaleDateString('en-US', {
    weekday: 'short', year: 'numeric', month: 'short', day: 'numeric'
  });

  status: StatusSnapshot | null = null;

  // Top-level nav items
  menus = [
    { id: 'dashboard', label: 'Dashboard', icon: 'dashboard' },
    { id: 'vpn',       label: 'VPN',       icon: 'network-globe' },
    { id: 'wan',       label: 'WAN',       icon: 'world' },
    { id: 'routing',   label: 'Routing',   icon: 'arrow-switch' },
    { id: 'lwm2m',     label: 'LwM2M',     icon: 'devices' },
    { id: 'services',  label: 'Services',  icon: 'cog' },
    { id: 'logs',      label: 'Logs',      icon: 'file' },
  ];

  constructor(
    private http: HttpsvcService,
    private session: SessionService,
    private router: Router,
    private pubsub: PubSubService
  ) {}

  ngOnInit(): void {
    this.refreshStatus();
  }

  onMenuSelect(menuId: string): void {
    this.selectedMenu = menuId;
    this.selectedSubNav = '';
    this.pubsub.publish('menu', menuId);
  }

  onSubNavSelect(item: string): void {
    this.selectedSubNav = item;
    this.pubsub.publish('subnav', { menu: this.selectedMenu, item });
  }

  refreshStatus(): void {
    this.http.getStatus().subscribe({
      next: (s) => {
        this.status = s;
        this.pubsub.publish('status', s);
      }
    });
  }

  onLogout(): void {
    this.http.logout().subscribe({
      next: () => {
        this.session.clear();
        this.router.navigate(['/login']);
      },
      error: () => {
        this.session.clear();
        this.router.navigate(['/login']);
      }
    });
  }

  // Quick status helpers
  get vpnState(): string { return this.status?.vpn?.state || 'unknown'; }
  get wifiState(): string { return this.status?.wifi?.state || 'unknown'; }
  get wanIface(): string { return this.status?.wan?.active_iface || '-'; }
  get serviceCount(): number {
    if (!this.status?.services) return 0;
    let n = 0;
    for (const s of Object.values(this.status.services)) {
      if (s?.state === 'running') n++;
    }
    return n;
  }
}
