import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-services-submenu',
  template: `
    <nav class="subnav">
      <a class="subnav-item" [class.active]="active==='list'" (click)="s('list')">All Services</a>
    </nav>
  `,
  styles: [`
    .subnav { display:flex; background:#16213e; padding:0 1rem; border-bottom:1px solid #1a5276; }
    .subnav-item { padding:10px 16px; color:#bdbdbd; cursor:pointer; font-size:13px; border-bottom:2px solid transparent; }
    .subnav-item:hover { color:#e0e0e0; }
    .subnav-item.active { color:#66bb6a; border-bottom-color:#2e7d32; }
  `]
})
export class ServicesSubmenuComponent {
  active = 'list';
  @Output() nav = new EventEmitter<string>();
  s(item: string): void { this.active = item; this.nav.emit(item); }
}
