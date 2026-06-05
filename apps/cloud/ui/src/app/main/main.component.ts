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
  menus = [
    { id: 'dashboard', label: 'Dashboard', svg: 'assets/icons/dashboard.svg' },
    { id: 'endpoints', label: 'Endpoints', svg: 'assets/icons/routing.svg' },
    { id: 'provision', label: 'Provision', svg: 'assets/icons/services.svg' },
    { id: 'logs',     label: 'Logs',     svg: 'assets/icons/logs.svg' },
  ];

  constructor(private http: HttpsvcService, private session: SessionService,
              private router: Router, public theme: ThemeService) { this.theme.init(); }

  onMenuSelect(id: string): void { this.selectedMenu = id; }
  onLogout(): void { this.http.logout().subscribe({ next: () => { this.session.clear(); this.router.navigate(['/login']); } }); }
}
