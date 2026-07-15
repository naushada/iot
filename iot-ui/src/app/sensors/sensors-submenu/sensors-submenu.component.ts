import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-sensors-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'env'"
         (click)="select('env')">
        <clr-icon shape="thermometer"></clr-icon>
        <span>Sensors</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'gps'"
         (click)="select('gps')">
        <clr-icon shape="map-marker"></clr-icon>
        <span>Location (GPS)</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class SensorsSubmenuComponent {
  active = 'env';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
