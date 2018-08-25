#include "vehicle.h"

#include "coordinate_conversions.h"
#include "map.h"
#include "output.h"
#include "game.h"
#include "item.h"
#include "veh_interact.h"
#include "cursesdef.h"
#include "catacharset.h"
#include "messages.h"
#include "iexamine.h"
#include "vpart_position.h"
#include "vpart_reference.h"
#include "string_formatter.h"
#include "ui.h"
#include "debug.h"
#include "sounds.h"
#include "translations.h"
#include "ammo.h"
#include "options.h"
#include "monster.h"
#include "npc.h"
#include "veh_type.h"
#include "itype.h"
#include "weather.h"
#include "cata_utility.h"

#include <sstream>
#include <stdlib.h>
#include <set>
#include <queue>
#include <math.h>
#include <array>
#include <numeric>
#include <algorithm>
#include <cassert>


static const itype_id fuel_type_none( "null" );
static const itype_id fuel_type_gasoline( "gasoline" );
static const itype_id fuel_type_diesel( "diesel" );
static const itype_id fuel_type_battery( "battery" );
static const itype_id fuel_type_water( "water_clean" );
static const itype_id fuel_type_muscle( "muscle" );

/*-----------------------------------------------------------------------------
 *                              VEHICLE_PART
 *-----------------------------------------------------------------------------*/
vehicle_part::vehicle_part()
    : mount( 0, 0 ), id( vpart_id::NULL_ID() ) {}

vehicle_part::vehicle_part( const vpart_id &vp, int const dx, int const dy, item &&obj )
    : mount( dx, dy ), id( vp ), base( std::move( obj ) )
{
    // Mark base item as being installed as a vehicle part
    base.item_tags.insert( "VEHICLE" );

    if( base.typeId() != vp->item ) {
        debugmsg( "incorrect vehicle part item, expected: %s, received: %s",
                  vp->item.c_str(), base.typeId().c_str() );
    }
}

vehicle_part::operator bool() const
{
    return id != vpart_id::NULL_ID();
}

const item &vehicle_part::get_base() const
{
    return base;
}

void vehicle_part::set_base( const item &new_base )
{
    base = new_base;
}

item vehicle_part::properties_to_item() const
{
    item tmp = base;
    tmp.item_tags.erase( "VEHICLE" );

    // Cables get special handling: their target coordinates need to remain
    // stored, and if a cable actually drops, it should be half-connected.
    if( tmp.has_flag( "CABLE_SPOOL" ) ) {
        tripoint local_pos = g->m.getlocal( target.first );
        if( !g->m.veh_at( local_pos ) ) {
            tmp.item_tags.insert( "NO_DROP" ); // That vehicle ain't there no more.
        }

        tmp.set_var( "source_x", target.first.x );
        tmp.set_var( "source_y", target.first.y );
        tmp.set_var( "source_z", target.first.z );
        tmp.set_var( "state", "pay_out_cable" );
        tmp.active = true;
    }

    return tmp;
}

std::string vehicle_part::name() const
{
    auto res = info().name();

    if( base.engine_displacement() > 0 ) {
        res.insert( 0, string_format( _( "%2.1fL " ), base.engine_displacement() / 100.0 ) );

    } else if( wheel_diameter() > 0 ) {
        res.insert( 0, string_format( _( "%d\" " ), wheel_diameter() ) );
    }

    if( base.is_faulty() ) {
        res += ( _( " (faulty)" ) );
    }

    if( base.has_var( "contained_name" ) ) {
        res += string_format( _( " holding %s" ), base.get_var( "contained_name" ) );
    }
    return res;
}

int vehicle_part::hp() const
{
    double dur = info().durability;
    return dur * health_percent();
}

float vehicle_part::damage() const
{
    return base.damage();
}

double vehicle_part::health_percent() const
{
    return ( 1.0 - ( double )base.damage() / base.max_damage() );
}

double vehicle_part::damage_percent() const
{
    return ( double )base.damage() / base.max_damage();
}

/** parts are considered broken at zero health */
bool vehicle_part::is_broken() const
{
    return base.damage() >= base.max_damage();
}

itype_id vehicle_part::ammo_current() const
{
    if( is_battery() ) {
        return "battery";
    }

    if( is_reactor() || is_turret() ) {
        return base.ammo_current();
    }

    if( is_tank() && !base.contents.empty() ) {
        return base.contents.front().typeId();
    }

    if( is_engine() ) {
        return info().fuel_type != "muscle" ? info().fuel_type : "null";
    }

    return "null";
}

long vehicle_part::ammo_capacity() const
{
    if( is_battery() || is_reactor() || is_turret() ) {
        return base.ammo_capacity();
    }

    if( base.is_watertight_container() ) {
        return base.get_container_capacity() / std::max( item::find_type( ammo_current() )->volume,
                units::from_milliliter( 1 ) );
    }

    return 0;
}

long vehicle_part::ammo_remaining() const
{
    if( is_battery() || is_reactor() || is_turret() ) {
        return base.ammo_remaining();
    }

    if( base.is_watertight_container() ) {
        return base.contents.empty() ? 0 : base.contents.back().charges;
    }

    return 0;
}

int vehicle_part::ammo_set( const itype_id &ammo, long qty )
{
    if( is_turret() ) {
        return base.ammo_set( ammo, qty ).ammo_remaining();
    }

    if( is_battery() || is_reactor() ) {
        base.ammo_set( ammo, qty >= 0 ? qty : ammo_capacity() );
        return base.ammo_remaining();
    }

    const itype *liquid = item::find_type( ammo );
    if( is_tank() && liquid->phase == LIQUID ) {
        base.contents.clear();
        auto stack = units::legacy_volume_factor / std::max( liquid->stack_size, 1 );
        long limit = units::from_milliliter( ammo_capacity() ) / stack;
        base.emplace_back( ammo, calendar::turn, qty >= 0 ? std::min( qty, limit ) : limit );
        return qty;
    }

    return -1;
}

void vehicle_part::ammo_unset()
{
    if( is_battery() || is_reactor() || is_turret() ) {
        base.ammo_unset();

    } else if( is_tank() ) {
        base.contents.clear();
    }
}

long vehicle_part::ammo_consume( long qty, const tripoint &pos )
{
    if( is_battery() || is_reactor() ) {
        return base.ammo_consume( qty, pos );
    }

    int res = std::min( ammo_remaining(), qty );

    if( base.is_watertight_container() && !base.contents.empty() ) {
        item &liquid = base.contents.back();
        liquid.charges -= res;
        if( liquid.charges == 0 ) {
            base.contents.clear();
        }
    }

    return res;
}

float vehicle_part::consume_energy( const itype_id &ftype, float energy )
{
    if( base.contents.empty() || ( !is_battery() && !is_reactor() &&
                                   !base.is_watertight_container() ) ) {
        return 0.0f;
    }

    item &fuel = base.contents.back();
    if( fuel.typeId() != ftype ) {
        return 0.0f;
    }

    assert( fuel.is_fuel() );
    float energy_per_unit = fuel.fuel_energy();
    long charges_to_use = static_cast<int>( std::ceil( energy / energy_per_unit ) );
    if( charges_to_use > fuel.charges ) {
        long had_charges = fuel.charges;
        base.contents.clear();
        return had_charges * energy_per_unit;
    }

    fuel.charges -= charges_to_use;
    return charges_to_use * energy_per_unit;
}

bool vehicle_part::can_reload( const itype_id &obj ) const
{
    // first check part is not destroyed and can contain ammo
    if( is_broken() || ammo_capacity() <= 0 ) {
        return false;
    }

    if( is_reactor() ) {
        return base.is_reloadable_with( obj );
    }

    if( is_tank() ) {
        if( !obj.empty() ) {
            // forbid filling tanks with non-liquids
            if( item::find_type( obj )->phase != LIQUID ) {
                return false;
            }
            // prevent mixing of different liquids
            if( ammo_current() != "null" && ammo_current() != obj ) {
                return false;
            }
        }
        // For tanks with set type, prevent filling with different types
        if( info().fuel_type != fuel_type_none && info().fuel_type != obj ) {
            return false;
        }
        return ammo_remaining() < ammo_capacity();
    }

    return false;
}

bool vehicle_part::fill_with( item &liquid, long qty )
{
    if( liquid.active || liquid.rotten() ) {
        // cannot refill using active liquids (those that rot) due to #18570
        return false;
    }

    if( !is_tank() || !can_reload( liquid.typeId() ) ) {
        return false;
    }

    base.fill_with( liquid, qty );
    return true;
}

const std::set<fault_id> &vehicle_part::faults() const
{
    return base.faults;
}

std::set<fault_id> vehicle_part::faults_potential() const
{
    return base.faults_potential();
}

bool vehicle_part::fault_set( const fault_id &f )
{
    if( !faults_potential().count( f ) ) {
        return false;
    }
    base.faults.insert( f );
    return true;
}

int vehicle_part::wheel_area() const
{
    return base.is_wheel() ? base.type->wheel->diameter * base.type->wheel->width : 0;
}

/** Get wheel diameter (inches) or return 0 if part is not wheel */
int vehicle_part::wheel_diameter() const
{
    return base.is_wheel() ? base.type->wheel->diameter : 0;
}

/** Get wheel width (inches) or return 0 if part is not wheel */
int vehicle_part::wheel_width() const
{
    return base.is_wheel() ? base.type->wheel->width : 0;
}

npc *vehicle_part::crew() const
{
    if( is_broken() || crew_id < 0 ) {
        return nullptr;
    }

    npc *const res = g->critter_by_id<npc>( crew_id );
    if( !res ) {
        return nullptr;
    }
    return res->is_friend() ? res : nullptr;
}

bool vehicle_part::set_crew( const npc &who )
{
    if( who.is_dead_state() || !who.is_friend() ) {
        return false;
    }
    if( is_broken() || ( !is_seat() && !is_turret() ) ) {
        return false;
    }
    crew_id = who.getID();
    return true;
}

void vehicle_part::unset_crew()
{
    crew_id = -1;
}

void vehicle_part::reset_target( const tripoint &pos )
{
    target.first = pos;
    target.second = pos;
}

bool vehicle_part::is_engine() const
{
    return info().has_flag( VPFLAG_ENGINE );
}

bool vehicle_part::is_light() const
{
    const auto &vp = info();
    return vp.has_flag( VPFLAG_CONE_LIGHT ) ||
           vp.has_flag( VPFLAG_CIRCLE_LIGHT ) ||
           vp.has_flag( VPFLAG_AISLE_LIGHT ) ||
           vp.has_flag( VPFLAG_DOME_LIGHT ) ||
           vp.has_flag( VPFLAG_ATOMIC_LIGHT );
}

bool vehicle_part::is_tank() const
{
    return base.is_watertight_container();
}

bool vehicle_part::is_battery() const
{
    return base.is_magazine() && base.ammo_type() == "battery";
}

bool vehicle_part::is_reactor() const
{
    return info().has_flag( "REACTOR" );
}

bool vehicle_part::is_turret() const
{
    return base.is_gun();
}

bool vehicle_part::is_seat() const
{
    return info().has_flag( "SEAT" );
}

const vpart_info &vehicle_part::info() const
{
    if( !info_cache ) {
        info_cache = &id.obj();
    }
    return *info_cache;
}

void vehicle::set_hp( vehicle_part &pt, int qty )
{
    if( qty == pt.info().durability ) {
        pt.base.set_damage( 0 );

    } else if( qty == 0 ) {
        pt.base.set_damage( pt.base.max_damage() );

    } else {
        double k = pt.base.max_damage() / double( pt.info().durability );
        pt.base.set_damage( pt.base.max_damage() - ( qty * k ) );
    }
}

bool vehicle::mod_hp( vehicle_part &pt, int qty, damage_type dt )
{
    double k = pt.base.max_damage() / double( pt.info().durability );
    return pt.base.mod_damage( - qty * k, dt );
}

bool vehicle::can_enable( const vehicle_part &pt, bool alert ) const
{
    if( std::none_of( parts.begin(), parts.end(), [&pt]( const vehicle_part & e ) {
    return &e == &pt;
} ) || pt.removed ) {
        debugmsg( "Cannot enable removed or non-existent part" );
    }

    if( pt.is_broken() ) {
        return false;
    }

    if( pt.info().has_flag( "PLANTER" ) && !warm_enough_to_plant() ) {
        if( alert ) {
            add_msg( m_bad, _( "It is too cold to plant anything now." ) );
        }
        return false;
    }

    // @todo: check fuel for combustion engines

    if( pt.info().epower < 0 && fuel_left( fuel_type_battery, true ) <= 0 ) {
        if( alert ) {
            add_msg( m_bad, _( "Insufficient power to enable %s" ), pt.name().c_str() );
        }
        return false;
    }

    return true;
}

bool vehicle::assign_seat( vehicle_part &pt, const npc &who )
{
    if( !pt.is_seat() || !pt.set_crew( who ) ) {
        return false;
    }

    // NPC's can only be assigned to one seat in the vehicle
    for( auto &e : parts ) {
        if( &e == &pt ) {
            continue; // skip this part
        }

        if( e.is_seat() ) {
            const npc *n = e.crew();
            if( n && n->getID() == who.getID() ) {
                e.unset_crew();
            }
        }
    }

    return true;
}

