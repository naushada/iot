import { enableProdMode } from '@angular/core';
import { platformBrowserDynamic } from '@angular/platform-browser-dynamic';
import { AppModule } from './app/app.module';
import { environment } from './environments/environment';

// Register the legacy <clr-icon> custom element + all icon shapes. Clarity's
// icon CSS is loaded via angular.json, but the icon JS must be imported
// explicitly — without it every <clr-icon> renders blank.
import '@clr/icons';
import '@clr/icons/shapes/all-shapes';

if (environment.production) {
  enableProdMode();
}

platformBrowserDynamic().bootstrapModule(AppModule)
  .catch(err => console.error(err));
