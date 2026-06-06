import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-services-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active==='list'"
         (click)="s('list')">
        <clr-icon shape="applications"></clr-icon>
        <span>All Services</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class ServicesSubmenuComponent {
  active = 'list';
  @Output() nav = new EventEmitter<string>();
  s(item: string): void { this.active = item; this.nav.emit(item); }
}
