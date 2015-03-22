#include "interface/Integrand.h"

MEM::Integrand::Integrand(int debug, const MEMConfig& config){

  // establish invariants
  debug_code         = debug;
  error_code         = 0;
  num_of_vars        = 0;
  ps_dim             = 0;
  naive_jet_counting = 0;
  extra_jets         = 0;
  fs                 = FinalState::Undefined;
  hypo               = Hypothesis::Undefined;
  ig2                = nullptr;
  n_calls            = 0;
  n_max_calls        = 0;
  n_skip             = 0;
  this_perm          = 0;
  cfg                = config;

  // init PDF set
  LHAPDF::initPDFSet(1, cfg.pdfset);

  if( debug_code&DebugVerbosity::init )  
    cout << "Integrand::Integrand(): START" << endl;
}

MEM::Integrand::~Integrand(){
  if( debug_code&DebugVerbosity::init )  
    cout << "Integrand::~Integrand()" << endl;    
  obs_jets.clear();
  obs_leptons.clear();
  obs_mets.clear();
  perm_indexes.clear();
  perm_indexes_assumption.clear();
  perm_const_assumption.clear();
  map_to_var.clear();
  map_to_part.clear();
}


/* 
   Initialise parameters (***once per event***)
   - determine final state 
   - save jet informations 
   - create list of permutations
   - determine number of variables
*/
void MEM::Integrand::init( const MEM::FinalState::FinalState f, const MEM::Hypothesis::Hypothesis h){

  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::init(): START" << endl;
  }    

  // set hypothesis to be tested
  hypo = h;
  fs   = f;

  assert( int(obs_leptons.size())==( 0*(fs==FinalState::HH || fs==FinalState::TTH) 
				     + 1*(fs==FinalState::LH) 
				     + 2*(fs==FinalState::LL) ) );

  // a class member: used to keep track of how many quarks are expected
  naive_jet_counting = 
    8*(fs==FinalState::HH) + 
    6*(fs==FinalState::LH) + 
    4*(fs==FinalState::LL) + 
    0*(fs==FinalState::TTH);

  // deal with jets:
  // if less jets are recorded than the naive_jet_counting,
  // fill in perm_index with -1;  
  size_t n_jets = obs_jets.size();
  vector<int> perm_index{};
  for(size_t id = 0; id < n_jets ; ++id) 
    perm_index.push_back( id );

  while( perm_index.size() < naive_jet_counting)
    perm_index.push_back( -1 );

  // calculate upper / lower edges
  for( auto j : obs_jets ){
    double y[2] = { j->p4().E(), j->p4().Eta() };
    pair<double, double> edges;    
    if( !j->isSet(Observable::E_LOW_Q) || !j->isSet(Observable::E_HIGH_Q) ){
      edges = get_support( y, TFType::qReco, cfg.j_range_CL,  debug_code ) ;
      j->addObs( Observable::E_LOW_Q,  edges.first  );
      j->addObs( Observable::E_HIGH_Q, edges.second );
    }
    if( !j->isSet(Observable::E_LOW_B) || !j->isSet(Observable::E_HIGH_B) ){
      edges = get_support( y, TFType::bReco, cfg.b_range_CL , debug_code) ;
      j->addObs( Observable::E_LOW_B,  edges.first  );
      j->addObs( Observable::E_HIGH_B, edges.second );    
    }
  }
  
  size_t n_perm{0};
  CompPerm comparator(cfg.highpt_first);
  sort( perm_index.begin(), perm_index.end(), comparator );  
  if( debug_code&DebugVerbosity::init ){
    cout << "\tIndexes to be permuted: [ " ;
    for( auto ind : perm_index ) cout << ind << " ";
    cout << "]" << endl;
  }
  do{
    perm_indexes.push_back( perm_index );
    if( debug_code&DebugVerbosity::init_more ) {
      cout << "\tperm. " << n_perm << ": [ ";
      for( auto ind : perm_index ) cout << ind << " ";
      cout << "]" << endl;
    }
    ++n_perm;
  } while( next_permutation( perm_index.begin(), perm_index.end(), comparator) );

  if( debug_code&DebugVerbosity::init ){
    cout << "\tTotal of " << n_perm << " permutation(s) created" << endl;
  }
  
  // Formula to get the number of unknowns
  // The number of variables is equal to npar - 2*extra_jets
  int unstable = (fs==FinalState::LH || fs==FinalState::LL || fs==FinalState::HH);
  ps_dim = 8*unstable + (3*(hypo==Hypothesis::TTH) + 4*(hypo==Hypothesis::TTBB))*(!unstable);
  
  num_of_vars = 
    // dimension of the phase-space
    3*ps_dim
    // leptons
    -3*obs_leptons.size()               
    // jet directions
    //-2*( TMath::Min(obs_jets.size(), naive_jet_counting) ) 
    -2*naive_jet_counting 
    // top/W mass
    -4*(unstable)               
    // H mass
    -1*(hypo==Hypothesis::TTH && unstable)
    // Px/Py
    -2*(obs_mets.size()==0);

  if( debug_code&DebugVerbosity::init ){
    cout << "\tTotal of " << num_of_vars << " unknowns (does not take into account lost jets)" << endl;
    cout << "\tIntegration code: " << cfg.int_code << endl;
  }

  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::init(): END" << endl;
  }
  return;
}

void MEM::Integrand::get_edges(double* lim, const std::vector<PSVar::PSVar>& lost, const size_t& nvar, const size_t& edge){

  // convention is: 
  //   even <=> cosTheta [-1,   +1]
  //   odd  <=> phi      [-PI, +PI]
  size_t count_extra{0};
  pair<double, double> phi_edges = {-TMath::Pi(),+TMath::Pi()};
  double y[2] = { obs_mets[0]->p4().Px(), obs_mets[0]->p4().Py()} ;

  switch( fs ){
  case FinalState::LH:
    if( cfg.m_range_CL<1. )
      phi_edges = get_support( y, TFType::MET, (edge? +cfg.m_range_CL : -cfg.m_range_CL),  debug_code ) ;
    lim[map_to_var[PSVar::E_q1]]      =  edge ?  1. :  0.;
    lim[map_to_var[PSVar::cos_qbar2]] =  edge ? +1  : -1.;
    lim[map_to_var[PSVar::phi_qbar2]] =  edge ? phi_edges.second : phi_edges.first;
    lim[map_to_var[PSVar::E_b]]       =  edge ?  1. :  0.;
    if( hypo==Hypothesis::TTBB ) 
      lim[map_to_var[PSVar::E_bbar]]  =  edge ?  1. :  0.;
    for( auto l = lost.begin() ; l !=lost.end(); ++l ){
      if(edge)
	lim[map_to_var[*l]] = count_extra%2==0 ? +1 : +TMath::Pi(); 
      else
	lim[map_to_var[*l]] = count_extra%2==0 ? -1 : -TMath::Pi(); 
      ++count_extra;
    }          
    break;
  case FinalState::LL:
    lim[map_to_var[PSVar::cos_qbar1]] =  edge ? +1  : -1.;
    lim[map_to_var[PSVar::phi_qbar1]] =  edge ? +TMath::Pi() : -TMath::Pi();
    lim[map_to_var[PSVar::cos_qbar2]] =  edge ? +1  : -1.;
    lim[map_to_var[PSVar::phi_qbar2]] =  edge ? +TMath::Pi() : -TMath::Pi();
    lim[map_to_var[PSVar::E_b]]       =  edge ?  1. :  0.;
    if( hypo==Hypothesis::TTBB ) 
      lim[map_to_var[PSVar::E_bbar]]  =  edge ?  1. :  0.;
    for( auto l = lost.begin() ; l !=lost.end(); ++l ){
      if(edge)
	lim[map_to_var[*l]] = count_extra%2==0 ? +1 : +TMath::Pi(); 
      else
	lim[map_to_var[*l]] = count_extra%2==0 ? -1 : -TMath::Pi(); 
      ++count_extra;
    }          
    break;
  case FinalState::HH:
    lim[map_to_var[PSVar::E_q1]]      =  edge ?  1. :  0.;
    lim[map_to_var[PSVar::E_q2]]      =  edge ?  1. :  0.;
    lim[map_to_var[PSVar::E_b]]       =  edge ?  1. :  0.;
    if( hypo==Hypothesis::TTBB ) 
      lim[map_to_var[PSVar::E_bbar]]  =  edge ?  1. :  0.;
    for( auto l = lost.begin() ; l !=lost.end(); ++l ){
      if(edge)
	lim[map_to_var[*l]] = count_extra%2==0 ? +1 : +TMath::Pi(); 
      else
	lim[map_to_var[*l]] = count_extra%2==0 ? -1 : -TMath::Pi(); 
      ++count_extra;
    }          
    break;
  case FinalState::TTH:
    lim[map_to_var[PSVar::P_t]]      =  edge ?  cfg.emax :  0.;
    lim[map_to_var[PSVar::cos_t]]    =  edge ? +0.99  : -0.99; //0.9 corresponds to |eta|<4.5
    lim[map_to_var[PSVar::phi_t]]    =  edge ? +TMath::Pi() : -TMath::Pi();
    lim[map_to_var[PSVar::P_tbar]]   =  edge ?  cfg.emax :  0.;
    lim[map_to_var[PSVar::cos_tbar]] =  edge ? +0.99  : -0.99;
    lim[map_to_var[PSVar::phi_tbar]] =  edge ? +TMath::Pi() : -TMath::Pi();
    lim[map_to_var[PSVar::Pz_h]]     =  edge ?  cfg.emax/2 : -cfg.emax/2;
    break;
  default:
    break;
  }

  if( debug_code&DebugVerbosity::init ){
    cout << "\tIntegrand::get_edges(): SUMMARY" << endl;
    cout << (edge ? "\t\tH" : "\t\tL") << " edges: [ ";
    for(size_t i = 0 ; i < nvar ; ++i) cout << lim[i] << " " ;
    cout << "]" << endl;
  }
 
}

double MEM::Integrand::get_width(const double* xL, const double* xU, const size_t nvar){
  double out{1.};
  for(size_t i = 0; i < nvar ; ++i) out *= TMath::Abs( xU[i]-xL[i] );
  return out;
}

void MEM::Integrand::fill_map(const std::vector<PSVar::PSVar>& lost){
  
  size_t count_extra{0};
  switch( fs ){
  case FinalState::LH:
    // jet <-> quark
    map_to_part[PSPart::q2]         = 0; // lepton
    map_to_part[PSPart::q1]         = 0; // jet
    map_to_part[PSPart::qbar1]      = 1; // jet
    map_to_part[PSPart::b1]         = 2; // jet
    map_to_part[PSPart::b2]         = 3; // jet
    map_to_part[PSPart::b]          = 4; // jet
    map_to_part[PSPart::bbar]       = 5; // jet

    // PS variable <-> VEGAS variable
    map_to_var[PSVar::E_q1]         = 0; //Eq 
    map_to_var[PSVar::cos_qbar2]    = 1; //cosNu
    map_to_var[PSVar::phi_qbar2]    = 2; //phiNu
    map_to_var[PSVar::E_b]          = 3; //Eb
    if( hypo==Hypothesis::TTBB ) map_to_var[PSVar::E_bbar] = 4;
    count_extra = 4+(hypo==Hypothesis::TTBB);
    for( auto l = lost.begin() ; l!=lost.end() ; ++l ) map_to_var[*l] = count_extra++;
    break;

  case FinalState::LL:
    // jet <-> quark
    map_to_part[PSPart::q1]         = 0; // lepton
    map_to_part[PSPart::q2]         = 1; // lepton                                                                                           
    map_to_part[PSPart::b1]         = 0; // jet
    map_to_part[PSPart::b2]         = 1; // jet
    map_to_part[PSPart::b]          = 2; // jet
    map_to_part[PSPart::bbar]       = 3; // jet

    // PS variable <-> VEGAS variable
    map_to_var[PSVar::cos_qbar1]    = 0; //cosNu
    map_to_var[PSVar::phi_qbar1]    = 1; //phiNu
    map_to_var[PSVar::cos_qbar2]    = 2; //cosNu
    map_to_var[PSVar::phi_qbar2]    = 3; //phiNu
    map_to_var[PSVar::E_b]          = 4; //Eb
    if( hypo==Hypothesis::TTBB ) map_to_var[PSVar::E_bbar] = 5;
    count_extra = 5+(hypo==Hypothesis::TTBB);
    for( auto l = lost.begin() ; l!=lost.end() ; ++l ) map_to_var[*l] = count_extra++;
    break;

  case FinalState::HH:
    // jet <-> quark
    map_to_part[PSPart::q1]         = 0; // jet
    map_to_part[PSPart::qbar1]      = 1; // jet
    map_to_part[PSPart::b1]         = 2; // jet
    map_to_part[PSPart::q2]         = 3; // jet
    map_to_part[PSPart::qbar2]      = 4; // jet
    map_to_part[PSPart::b2]         = 5; // jet
    map_to_part[PSPart::b]          = 6; // jet
    map_to_part[PSPart::bbar]       = 7; // jet

    // PS variable <-> VEGAS variable
    map_to_var[PSVar::E_q1]         = 0; //Eq 
    map_to_var[PSVar::E_q2]         = 1; //Eq
    map_to_var[PSVar::E_b]          = 2; //Eb
    if( hypo==Hypothesis::TTBB ) map_to_var[PSVar::E_bbar] = 3;
    count_extra = 3+(hypo==Hypothesis::TTBB);
    for( auto l = lost.begin() ; l!=lost.end() ; ++l ) map_to_var[*l] = count_extra++;
    break;

  case FinalState::TTH:
    // PS variable <-> VEGAS variable
    map_to_var[PSVar::P_t]          = 0;
    map_to_var[PSVar::cos_t]        = 1;
    map_to_var[PSVar::phi_t]        = 2;
    map_to_var[PSVar::P_tbar]       = 3;
    map_to_var[PSVar::cos_tbar]     = 4;
    map_to_var[PSVar::phi_tbar]     = 5;
    map_to_var[PSVar::Pz_h]         = 6;
  default:
    break;
  }

  if( debug_code&DebugVerbosity::init_more ){
    cout << "\tIntegrand::fill_map(): SUMMARY" << endl;
    cout << "\tMapping between phase-space variables and VEGAS variables:" << endl;
    for( auto iter = map_to_var.begin() ; iter!=map_to_var.end() ; ++iter ){
      cout << "\t\tPS[" << static_cast<int>(iter->first) << "] maps to x[" << iter->second << "]" << endl;
    }
    cout << "\tMapping between quarks and objects in the permuted obs_ collections:" << endl;
    for( auto iter = map_to_part.begin() ; iter!=map_to_part.end() ; ++iter ){
      cout << "\t\tParticle (" << static_cast<int>(iter->first) << ") maps to position " << iter->second << endl;
    }
  }

  return;
}

void MEM::Integrand::push_back_object(const LV& p4,  const MEM::ObjectType::ObjectType& type){

  Object* obj = new Object(p4, type);

  switch( type ){
  case ObjectType::Jet:
    obs_jets.push_back( obj ); 
    break;
  case ObjectType::Lepton:
    obs_leptons.push_back( obj ); 
    break;
  case ObjectType::MET:
    obs_mets.push_back( obj ); 
    break;
  default:
    cout << "*** MEM::Intgrator::push_back_object(): Unknown type of object added" << endl;
    break;
  }

  if( debug_code&DebugVerbosity::init_more ){
    cout << "Integrand::push_back_object(): SUMMARY" << endl;
    obj->print(cout);
  }
  
  return;
}

void MEM::Integrand::push_back_object(MEM::Object* obj){

  switch( obj->type() ){
  case ObjectType::Jet:
    obs_jets.push_back( obj ); 
    break;
  case ObjectType::Lepton:
    obs_leptons.push_back( obj ); 
    break;
  case ObjectType::MET:
    obs_mets.push_back( obj ); 
    break;
  default:
    cout << "*** MEM::Intgrator::push_back_object(): Unknown type of object added" << endl;
    break;
  }

  if( debug_code&DebugVerbosity::init_more ){
    cout << "Integrand::fill_map(): SUMMARY" << endl;
    obj->print(cout);
  }
  
  return;
}

void MEM::Integrand::add_object_observable( const std::pair<MEM::Observable::Observable, double>& obs, const ObjectType::ObjectType& type ){
  
  switch( type ){
  case ObjectType::Jet :
    if(obs_jets.size()>0)    
      (obs_jets.back())->addObs( obs.first, obs.second );
    break;
  case ObjectType::Lepton :
    if(obs_leptons.size()>0) 
      (obs_leptons.back())->addObs( obs.first, obs.second );
    break;
  case ObjectType::MET:
    if(obs_mets.size()>0) 
      (obs_mets.back())->addObs( obs.first, obs.second );
    break;
  default:
    cout << "Integrand::add_object_observables(): Unknown type of object added" << endl;
    break;
  }

  return;
}

void MEM::Integrand::set_integrand(const int code){
  cfg.int_code = code;
}

void MEM::Integrand::set_ncalls(const size_t& n){
  cfg.n_max_calls = n;
  cfg.is_default  = false;
}

void MEM::Integrand::set_sqrts(const double& s){
  cfg.sqrts = s;
}

void MEM::Integrand::set_cfg(const MEMConfig& config){
  cfg = config;
}

//void MEM::Integrand::set_permutation_strategy(const std::vector<MEM::Permutations>& str){
void MEM::Integrand::set_permutation_strategy(const std::vector<MEM::Permutations::Permutations>& str){
  cfg.perm_pruning = str; 
}


MEM::MEMOutput MEM::Integrand::run( const MEM::FinalState::FinalState f, const MEM::Hypothesis::Hypothesis h, const std::vector<MEM::PSVar::PSVar> list){

  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::run(): START" << endl;
  }

  // the output for this evaluation
  MEMOutput out;

  // start the clock....
  auto t0 = high_resolution_clock::now();
  
  // prepare permutation, count variables
  init(f,h);

  // number of calls
  n_max_calls = cfg.is_default ? 
    cfg.calls[static_cast<std::size_t>(fs)][static_cast<std::size_t>(h)][list.size()/2] : 
    cfg.n_max_calls;

  // create integrator
  ig2 = new ROOT::Math::GSLMCIntegrator(ROOT::Math::IntegrationMultiDim::kVEGAS, cfg.abs, cfg.rel, n_max_calls);

  if( debug_code&DebugVerbosity::init_more ){
    ig2->Options().Print(std::cout);
    ig2->ExtraOptions()->Print(std::cout);
  }

  // start the clock....
  auto t1 = high_resolution_clock::now();

  // do the calculation
  make_assumption( list, out ); 

  // stop the clock!
  auto t2 = high_resolution_clock::now();

  out.time          = static_cast<int>(duration_cast<milliseconds>(t2-t1).count());
  out.num_perm      = perm_indexes_assumption.size();
  out.final_state   = fs;
  out.hypothesis    = h;
  out.assumption    = list.size()/2;
  out.num_max_calls = n_max_calls;
  out.num_calls     = n_calls;
  out.efficiency    = float(n_calls)/(n_calls+n_skip);
  out.error_code    = error_code;

  if( debug_code&DebugVerbosity::output ){
    out.print(cout);
  }

  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::run(): DONE in " << static_cast<int>(duration_cast<milliseconds>(t2-t0).count())*0.001 << " sec" << endl;
  }

  // delete stuff and prepare for new hypothesis
  next_hypo();

  return out;
}
void MEM::Integrand::next_event(){
  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::next_event(): START" << endl;
  }
  obs_jets.clear();
  obs_leptons.clear();
  obs_mets.clear();  
  error_code         = 0;
  num_of_vars        = 0;
  ps_dim             = 0;
  naive_jet_counting = 0;
  extra_jets         = 0;
  n_calls            = 0;
  n_skip             = 0;
  cfg.is_default     = true;
  perm_indexes.clear();
  perm_indexes_assumption.clear();
  perm_const_assumption.clear();
  map_to_var.clear();
  map_to_part.clear();
  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::next_event(): END" << endl;
  }
}

void MEM::Integrand::next_hypo(){
  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::next_hypo(): START" << endl;
  }
  if(ig2!=nullptr) delete ig2;
  perm_indexes.clear();
  perm_indexes_assumption.clear();
  perm_const_assumption.clear();
  map_to_var.clear();
  map_to_part.clear();
  n_calls = 0;
  n_skip  = 0;
  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::next_hypo(): END" << endl;
  }
}

bool MEM::Integrand::test_assumption( const size_t& lost){

  if( (obs_jets.size()+lost) < naive_jet_counting){
    if( debug_code&DebugVerbosity::init ) cout << "\t This assumption cannot be made: too few jets" << endl;
    return false;
  }
  extra_jets = (obs_jets.size()+lost-naive_jet_counting);
  return true;
}

void MEM::Integrand::make_assumption( const std::vector<MEM::PSVar::PSVar>& lost, MEMOutput& out){

  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::make_assumption(): START" << endl;
  }

  double prob{0.};
  double err2{0.};
  double chi2{0.};

  // an assumption may not be consistent with the number of observed jets
  // E.g.: assume 1 lost quark but 2 jets missing wrt to expectation
  // N.B. extra_jets filled here!!!
  if(!test_assumption(lost.size()/2)) return;

  perm_indexes_assumption.clear();

  // extra variables to integrate over  
  fill_map( lost );  

  // Remove unwanted permutations:
  //    CASE (1) ==> perm contains already -1: then -1 must be aligned with the lost quark
  //    CASE (2) ==> perm does not contain -1: then set the correct index to -1
  for( auto perm : perm_indexes ){    
    
    auto good_perm = perm;

    // - *it gives the integ. var. position in PSVar
    // - provide first cosTheta: then *it-1 gives the position of E
    // - (*it-1) / 3 gives particle position (0=q1,1=qbar1,2=b1,...)
    size_t count{0};
    for(auto it = lost.begin() ; it!=lost.end() ; ++count, ++it ){
      size_t lost_particle = (static_cast<size_t>(*it)-1)/3;
      if(count%2==0) perm[ lost_particle ] = -1;      
    }

    // count the number of lost quarks as assumed in perm
    // if it turns out to be equal to the assumed (lost.size()/2), 
    // then push back the permutation
    count = 0;
    for(auto ind : perm){ if(ind<0) ++count; }

    if(count!=(lost.size()/2))  continue;
    if( !accept_perm( perm, cfg.perm_pruning )) continue;
    
    perm_indexes_assumption.push_back( perm ); 
    perm_const_assumption.push_back( get_permutation_constants(perm) );
  }  

  if( debug_code&DebugVerbosity::init_more ) {
    size_t n_perm{0};
    for( auto perm : perm_indexes_assumption ){
      cout << "\tperm. " << n_perm << ", k-factor=" << perm_const_assumption[n_perm] << ": [ ";
      ++n_perm;
      for( auto ind : perm )
	cout << ind << " ";
      cout << "]" << endl;
    }
  }
  if( debug_code&DebugVerbosity::init ) 
    cout << "\tA total of " << perm_indexes_assumption.size() << " permutations have been considered for this assumption" << endl;

  // create integration ranges
  size_t npar = num_of_vars + lost.size();

  double xL[npar], xU[npar];
  get_edges(xL, lost, npar, 0);
  get_edges(xU, lost, npar, 1);      

  double volume = get_width(xL,xU,npar);
  
  // function
  ROOT::Math::Functor toIntegrate(this, &MEM::Integrand::Eval, npar);  
  ig2->SetFunction(toIntegrate);
  
  // do the integral
  if( cfg.perm_int ){
    for( std::size_t n_perm = 0; n_perm < perm_indexes_assumption.size() ; ++n_perm ){
      this_perm = n_perm;
      // create integrator
      delete ig2;
      ig2 = new ROOT::Math::GSLMCIntegrator(ROOT::Math::IntegrationMultiDim::kVEGAS, cfg.abs, cfg.rel, n_max_calls);
      ig2->SetFunction(toIntegrate);
      double n_prob = ig2->Integral(xL,xU);
      if( debug_code&DebugVerbosity::output ){
	cout << "\tPermutation num. " << this_perm << " returned p=" << n_prob << endl;
      }
      prob += n_prob;
      err2 += TMath::Power(ig2->Error(),2.);
      chi2 += ig2->ChiSqr()/perm_indexes_assumption.size();
    }
  }

  // do the integral
  if( !cfg.perm_int ){
    prob += ig2->Integral(xL,xU);
    err2 += TMath::Power(ig2->Error(),2.);
    chi2 += ig2->ChiSqr();
  }
  
  if( debug_code&DebugVerbosity::init ){
    cout << "Integrand::make_assumption(): END" << endl;
  }

  if(!cfg.int_code) prob /= volume;  
  out.p     = prob;
  out.p_err = sqrt(err2);
  out.chi2  = chi2;

  return;
}

bool MEM::Integrand::accept_perm( const vector<int>& perm, const std::vector<MEM::Permutations::Permutations>& strategies ) const {

  if( debug_code&DebugVerbosity::init_more ){
    cout << "Integrand::accept_perm(): START" << endl;
  }

  // helper containers
  vector<size_t> indexes1;
  vector<size_t> indexes2;

  // loop over strategies to filter out permutations
  for( auto s = strategies.begin() ; s != strategies.end() ; ++s){

    if( debug_code&DebugVerbosity::init_more ){
      cout << "\tUsing strategy Permutations::" << static_cast<size_t>(*s) << "..." << endl;
    }

    switch( *s ){

      // Require all b quarks to be matched to tagged jets
    case Permutations::BTagged:
      indexes1 =  vector<size_t>{map_to_part.find(PSPart::b1)->second, 
				 map_to_part.find(PSPart::b2)->second, 
				 map_to_part.find(PSPart::b)->second, 
				 map_to_part.find(PSPart::bbar)->second};
      for( auto ind : indexes1 ){ 
	if( perm[ind]>=0 && 
	    obs_jets[perm[ind]]->isSet(Observable::BTAG) && 
	    obs_jets[perm[ind]]->getObs(Observable::BTAG)<0.5){
	  if( debug_code&DebugVerbosity::init_more ){
	    cout << "\t\tDiscard permutation: obs_jets[ perm[" << ind << "] ] has BTAG=" << obs_jets[perm[ind]]->getObs(Observable::BTAG) << endl;
	  }
	  return false;
	}
      }
      break;

      // Require all non-b quarks to be matched to untagged jets
    case Permutations::QUntagged:
      indexes1.clear();
      if( fs==FinalState::LH ) indexes1 = vector<size_t>{map_to_part.find(PSPart::q1)->second, 
							 map_to_part.find(PSPart::qbar1)->second};
      if( fs==FinalState::LL ) indexes1 = vector<size_t>{};
      if( fs==FinalState::HH ) indexes1  = vector<size_t>{map_to_part.find(PSPart::q1)->second,
							  map_to_part.find(PSPart::qbar1)->second, 
							  map_to_part.find(PSPart::q2)->second,
                                                          map_to_part.find(PSPart::qbar2)->second};
      for( auto ind : indexes1 ){
	if( perm[ind]>=0 &&
            obs_jets[perm[ind]]->isSet(Observable::BTAG) &&
            obs_jets[perm[ind]]->getObs(Observable::BTAG)>0.5){
	  if( debug_code&DebugVerbosity::init_more ){
	    cout << "\t\tDiscard permutation: obs_jets[ perm[" << ind << "] ] has BTAG=" << obs_jets[perm[ind]]->getObs(Observable::BTAG) << endl;
	  }
	  return false;  
	}
      }
      break;
      
      // require that no other permutations has been already considered
      // differing from perm by swapping the W quarks
    case Permutations::QQbarSymmetry:
    case Permutations::BBbarSymmetry:
      indexes1.clear();
      indexes2.clear();
      if( fs==FinalState::LH ){
	// the  symmetric part
	indexes1 = vector<size_t>{ map_to_part.find(PSPart::q1)->second, 
				   map_to_part.find(PSPart::qbar1)->second};
	if( (*s)==Permutations::BBbarSymmetry){
	  indexes1.push_back(map_to_part.find(PSPart::b)->second);
	  indexes1.push_back(map_to_part.find(PSPart::bbar)->second);
	}
	// the asymmetric part
	indexes2 = vector<size_t>{ map_to_part.find(PSPart::b1)->second,
				   map_to_part.find(PSPart::b2)->second, };  
      }
      if( fs==FinalState::LL ){
	// the  symmetric part                                                                                                                      
	if( (*s)==Permutations::BBbarSymmetry){
	  indexes1.push_back(map_to_part.find(PSPart::b)->second);
	  indexes1.push_back(map_to_part.find(PSPart::bbar)->second);
	}
        // the asymmetric part                                                                                                                      
        indexes2 = vector<size_t>{ map_to_part.find(PSPart::b1)->second,
				   map_to_part.find(PSPart::b2)->second, };
      }
      if( fs==FinalState::HH ){
	// the  symmetric part
	indexes1 = vector<size_t>{ map_to_part.find(PSPart::q1)->second, 
				   map_to_part.find(PSPart::qbar1)->second, 
				   map_to_part.find(PSPart::q2)->second,
				   map_to_part.find(PSPart::qbar2)->second};
	if( (*s)==Permutations::BBbarSymmetry){
	  indexes1.push_back(map_to_part.find(PSPart::b)->second);
	  indexes1.push_back(map_to_part.find(PSPart::bbar)->second);
	}
	// the asymmetric part
	indexes2 = vector<size_t>{ map_to_part.find(PSPart::b1)->second,
				   map_to_part.find(PSPart::b2)->second, };  
      }

      for( auto visited : perm_indexes_assumption ){ 	
	bool asymmetric_part{true};
	bool symmetric_part {true};
	
	// loop over quark positions that should be matched to same jet
	for(auto same : indexes2 ) 
	  if(visited[same]!=perm[same]) asymmetric_part = false;

	// loop over quark position pairs searching for a swap
	for(size_t i = 0 ; i < (indexes1.size()/2) ; ++i){
	  bool same = (visited[ indexes1[2*i] ]==perm[indexes1[2*i]])   && (visited[ indexes1[2*i+1] ]==perm[indexes1[2*i+1]]);
	  bool swap = (visited[ indexes1[2*i] ]==perm[indexes1[2*i+1]]) && (visited[ indexes1[2*i+1] ]==perm[indexes1[2*i]]);
	  if( !(same || swap) ) symmetric_part = false;
	}

	if( asymmetric_part && symmetric_part ){
	  if( debug_code&DebugVerbosity::init_more ){
	    cout << "\t\tDiscard permutation: a swap has been found" << endl;
	  }
	  return false;
	}
      }
      break;
    default:
      break;
    }
    
  }
  
  return true;
}

double MEM::Integrand::get_permutation_constants( const vector<int>& perm ) const {

  if( debug_code&DebugVerbosity::init_more ){
    cout << "Integrand::get_permutation_constants(): START" << endl;
  }

  double p{1.};
  double DeltaE{1.};
  size_t pos;

  MEM::Object* obj = nullptr;
  switch( fs ){
  case FinalState::LH:
    // PSVar::E_q1
    pos = map_to_part.find(PSPart::q1)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_Q) - obj->getObs(Observable::E_LOW_Q));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_q = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_b
    pos = map_to_part.find(PSPart::b)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B)); 
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_b = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_bbar
    if( hypo==Hypothesis::TTBB ){
      pos = map_to_part.find(PSPart::bbar)->second;
      obj = obs_jets[ perm[pos] ];
      DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_bbar = " << DeltaE << " GeV" << endl;
      p *= DeltaE;
    }
    break;
  case FinalState::LL:
    // PSVar::E_b 
    pos = map_to_part.find(PSPart::b)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_b = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_bbar 
    if( hypo==Hypothesis::TTBB ){
      pos = map_to_part.find(PSPart::bbar)->second;
      obj = obs_jets[ perm[pos] ];
      DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B));
      if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_bbar = " << DeltaE << " GeV" << endl;
      p *= DeltaE;
    }
    break;

  case FinalState::HH:
    // PSVar::E_q1
    pos = map_to_part.find(PSPart::q1)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_Q) - obj->getObs(Observable::E_LOW_Q));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_q = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_q2
    pos = map_to_part.find(PSPart::q2)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_Q) - obj->getObs(Observable::E_LOW_Q));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_q = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_b
    pos = map_to_part.find(PSPart::b)->second;
    obj = obs_jets[ perm[pos] ];
    DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B)); 
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_b = " << DeltaE << " GeV" << endl;
    p *= DeltaE;

    // PSVar::E_bbar
    if( hypo==Hypothesis::TTBB ){
      pos = map_to_part.find(PSPart::bbar)->second;
      obj = obs_jets[ perm[pos] ];
      DeltaE = (obj->getObs(Observable::E_HIGH_B) - obj->getObs(Observable::E_LOW_B));
    if( debug_code&DebugVerbosity::init_more ) cout << "\tdE_bbar = " << DeltaE << " GeV" << endl;
      p *= DeltaE;
    }
    break;
  default:
    break;
  }

  if( debug_code&DebugVerbosity::init_more ){
    cout << "\tVariable transformation permutation-dependent returned p=" << p << endl;
  }

  if( debug_code&DebugVerbosity::init_more ){
    cout << "Integrand::get_permutation_constants(): END" << endl;
  }

  return p;
}


double MEM::Integrand::Eval(const double* x) const{

#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\tIntegrand::Eval(): START" << endl;
      cout << "\t\tVEGAS call num. " << n_calls << endl;
    }
#endif

  double p{0.};

  for( std::size_t n_perm = 0; n_perm < perm_indexes_assumption.size() ; ++n_perm ){
    if( cfg.perm_int ){
      if(n_perm != this_perm) continue;
    }
    double p0 = probability(x, n_perm );
    double p1 = perm_const_assumption[n_perm];
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\t\tPermutation #" << n_perm 
	   << " => p = (" << p0 << "*" << p1 << ") = " << (p0*p1) << endl;
      cout << "\t\t\tP --> " << p << " + " << (p0*p1) << endl;
    }
#endif
    p += (p0*p1);
  }
  
  ++(const_cast<Integrand*>(this)->n_calls);

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::Eval(): END" << endl;
  }
#endif

  return p;
}

int MEM::Integrand::create_PS(MEM::PS& ps, const double* x, const vector<int>& perm ) const {

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS(): START" << endl;
  }
#endif

  switch( fs ){
  case FinalState::LH:
    return create_PS_LH(ps, x, perm);
    break;
  case FinalState::LL:
    return create_PS_LL(ps, x, perm);
    break;
  case FinalState::HH:
    return create_PS_HH(ps, x, perm);
    break;
  case FinalState::TTH:
    return create_PS_TTH(ps, x, perm);
    break;
  default:
    break;
  }

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS(): END" << endl;
  }
#endif

  return 0;
}


int MEM::Integrand::create_PS_LH(MEM::PS& ps, const double* x, const vector<int>& perm ) const {

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_LH(): START" << endl;
  }
#endif

  // corrupted phase space
  int accept{0};

  // store temporary values to build four-vectors
  double E     {0.};
  double E_LOW {0.};
  double E_HIGH{0.};
  double E_REC {numeric_limits<double>::max()};
  TVector3 dir (1.,0.,0.);

  // map a quark to an index inside the obs_ collections
  size_t nj_q1    =  map_to_part.find(PSPart::q1)->second; 
  size_t nj_qbar1 =  map_to_part.find(PSPart::qbar1)->second; 
  size_t nj_b1    =  map_to_part.find(PSPart::b1)->second; 
  size_t nl_q2    =  map_to_part.find(PSPart::q2)->second; 
  size_t nj_b2    =  map_to_part.find(PSPart::b2)->second; 
  size_t nj_b     =  map_to_part.find(PSPart::b)->second;
  size_t nj_bbar  =  map_to_part.find(PSPart::bbar)->second;

  /////  PSPart::q1
  if( perm[ nj_q1 ]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_q1] ]; 
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_Q) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_Q);
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_q1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_q1)->second ] );
    E_LOW   = MQ;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_q1)->second ]);
  extend_PS( ps, PSPart::q1, E, MQ, dir, perm[nj_q1], PSVar::cos_q1, PSVar::phi_q1, PSVar::E_q1, (perm[nj_q1]>=0?TFType::qReco:TFType::qLost) ); 

  /////  PSPart::qbar1
  if( perm[nj_qbar1]>=0 ){
    dir     = obs_jets[ perm[nj_qbar1] ]->p4().Vect().Unit();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_qbar1)->second ] );
  }
  E    = solve( ps.lv(PSPart::q1), DMW2 , MQ, dir, E_REC, accept );
  extend_PS( ps, PSPart::qbar1, E, MQ, dir, perm[nj_qbar1], PSVar::cos_qbar1, PSVar::phi_qbar1, PSVar::E_qbar1,  (perm[nj_qbar1]>=0?TFType::qReco:TFType::qLost) ); 

  /////  PSPart::b1
  if( perm[nj_b1]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b1] ]; 
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b1)->second ] );
  }
  E       = solve(ps.lv(PSPart::q1) + ps.lv(PSPart::qbar1), DMT2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::b1, E, MB , dir, perm[nj_b1], PSVar::cos_b1, PSVar::phi_b1, PSVar::E_b1,  (perm[nj_b1]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::q2
  MEM::Object* lep = obs_leptons[ nl_q2 ]; 
  dir     = lep->p4().Vect().Unit(); 
  E       = lep->p4().E();
  extend_PS( ps, PSPart::q2, E, ML, dir, nl_q2, PSVar::cos_q2, PSVar::phi_q2, PSVar::E_q2, TFType::muReco, int(lep->getObs(Observable::CHARGE)) ); 
    
  /////  PSPart::qbar2
  dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar2)->second ]) );
  double phi_n = x[ map_to_var.find(PSVar::phi_qbar2)->second ] + obs_mets[0]->p4().Phi();
  if     ( phi_n>+PI ) phi_n -= 2*PI;
  else if( phi_n<-PI ) phi_n += 2*PI;
  dir.SetPhi  ( phi_n );
  E    = solve( ps.lv(PSPart::q2), DMW2, ML, dir, E_REC, accept );
  extend_PS( ps, PSPart::qbar2, E, 0., dir, -1, PSVar::cos_qbar2, PSVar::phi_qbar2, PSVar::E_qbar2, TFType::MET  ); 

  /////  PSPart::b2
  if( perm[nj_b2]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b2] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b2)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b2)->second ] );
  }
  E       = solve( ps.lv(PSPart::q2) + ps.lv(PSPart::qbar2), DMT2, MB, dir, E_REC, accept );
  extend_PS( ps, PSPart::b2, E, MB, dir, perm[nj_b2], PSVar::cos_b2, PSVar::phi_b2, PSVar::E_b2,  (perm[nj_b2]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::b
  if( perm[nj_b]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b] ];
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);    
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_b)->second ]);
  extend_PS( ps, PSPart::b, E, MB, dir, perm[nj_b], PSVar::cos_b, PSVar::phi_b, PSVar::E_b,  (perm[nj_b]>=0?TFType::bReco:TFType::bLost) ); 

  /////  PSPart::bbar   
  if( perm[nj_bbar]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_bbar] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);        
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_bbar)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_bbar)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E    = hypo==Hypothesis::TTBB ?  
    E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_bbar)->second ]) : 
    solve( ps.lv(PSPart::b), DMH2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::bbar, E, MB, dir, perm[nj_bbar], PSVar::cos_bbar, PSVar::phi_bbar, PSVar::E_bbar,  (perm[nj_bbar]>=0?TFType::bReco:TFType::bLost) ); 

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_LH(): END" << endl;
  }
#endif

  return accept;
}

void MEM::Integrand::extend_PS(MEM::PS& ps, const MEM::PSPart::PSPart& part, 
			       const double& E,  const double& M, const TVector3& dir,
			       const int& pos,  const PSVar::PSVar& var_cos, const PSVar::PSVar& var_phi, const PSVar::PSVar& var_E, 
			       const TFType::TFType& type, const int charge) const {

  double E_phys  = TMath::Max(E,M); 
  double P       = sqrt(E_phys*E_phys - M*M);
  ps.set(part,  MEM::GenPart(TLorentzVector( dir*P, E_phys ), type, charge ) );

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\t\tExtend phase-space point: adding variable " << static_cast<size_t>(part) << endl;
    if(  map_to_var.find(var_E)!=map_to_var.end() )
      cout << "\t\tE   = x[" <<  map_to_var.find(var_E)->second << "] = " << E << " GeV" << endl;
    else
      cout << "\t\tE   = SOLVE() = " << E << " GeV" << endl;
    if(   map_to_var.find(var_cos)!= map_to_var.end() && map_to_var.find(var_phi)!= map_to_var.end()  ){
      cout << "\t\tcos = x[" << map_to_var.find(var_cos)->second << "] = " << TMath::Cos(dir.Theta()) << endl;    
      cout << "\t\tphi = x[" << map_to_var.find(var_phi)->second << "] = " << dir.Phi() << endl;
    }
    else{
      cout << "\t\tUsing obs[" << pos << "]" << endl;
    }
  }
#endif

}

void MEM::Integrand::extend_PS_nodebug(MEM::PS& ps, const MEM::PSPart::PSPart& part, 
				       const double& E,  const double& M, const TVector3& dir) const {
  double E_phys  = TMath::Max(E,M); 
  double P       = sqrt(E_phys*E_phys - M*M);
  ps.set(part,  MEM::GenPart(TLorentzVector( dir*P, E_phys ), TFType::Unknown, 0 ) );
}


int MEM::Integrand::create_PS_LL(MEM::PS& ps, const double* x, const vector<int>& perm ) const {

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_LL(): START" << endl;
  }
#endif

  // corrupted phase space
  int accept{0};

  // store temporary values to build four-vectors
  double E     {0.};
  double E_LOW {0.};
  double E_HIGH{0.};
  double E_REC {numeric_limits<double>::max()};
  TVector3 dir (1.,0.,0.);

  // map a quark to an index inside the obs_ collections
  size_t nl_q1    =  map_to_part.find(PSPart::q1)->second; 
  size_t nj_b1    =  map_to_part.find(PSPart::b1)->second; 
  size_t nl_q2    =  map_to_part.find(PSPart::q2)->second; 
  size_t nj_b2    =  map_to_part.find(PSPart::b2)->second; 
  size_t nj_b     =  map_to_part.find(PSPart::b)->second;
  size_t nj_bbar  =  map_to_part.find(PSPart::bbar)->second;

  /////  PSPart::q1
  MEM::Object* lep1 = obs_leptons[ nl_q1 ];
  dir     = lep1->p4().Vect().Unit();
  E       = lep1->p4().E();
  extend_PS( ps, PSPart::q1, E, ML, dir, nl_q1, PSVar::cos_q1, PSVar::phi_q1, PSVar::E_q1, TFType::muReco, int(lep1->getObs(Observable::CHARGE)) ); 

  /////  PSPart::qbar1
  dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar1)->second ]) );
  dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_qbar1)->second ] );
  E    = solve( ps.lv(PSPart::q1), DMW2, ML, dir, E_REC, accept );
  extend_PS( ps, PSPart::qbar1, E, 0., dir, -1, PSVar::cos_qbar1, PSVar::phi_qbar1, PSVar::E_qbar1, TFType::MET  ); 

  /////  PSPart::b1
  if( perm[nj_b1]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b1] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b1)->second ] );
  }
  E       = solve(ps.lv(PSPart::q1) + ps.lv(PSPart::qbar1), DMT2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::b1, E, MB , dir, perm[nj_b1], PSVar::cos_b1, PSVar::phi_b1, PSVar::E_b1,  (perm[nj_b1]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::q2
  MEM::Object* lep2 = obs_leptons[ nl_q2 ];
  dir     = lep2->p4().Vect().Unit();
  E       = lep2->p4().E();
  extend_PS( ps, PSPart::q2, E, ML, dir, nl_q2, PSVar::cos_q2, PSVar::phi_q2, PSVar::E_q2, TFType::muReco, int(lep2->getObs(Observable::CHARGE)) ); 

  /////  PSPart::qbar2
  dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar2)->second ]) );
  dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_qbar2)->second ] );
  E    = solve( ps.lv(PSPart::q2), DMW2, ML, dir, E_REC, accept);
  extend_PS( ps, PSPart::qbar2, E, 0., dir, -1, PSVar::cos_qbar2, PSVar::phi_qbar2, PSVar::E_qbar2, TFType::MET  ); 

  //  PSVar::cos_b2, PSVar::phi_b2, PSVar::E_b2   
  if( perm[nj_b2]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b2] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b2)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b2)->second ] );
  }
  E       = solve( ps.lv(PSPart::q2) + ps.lv(PSPart::qbar2), DMT2, MB, dir, E_REC, accept );
  extend_PS( ps, PSPart::b2, E, MB, dir, perm[nj_b2], PSVar::cos_b2, PSVar::phi_b2, PSVar::E_b2,  (perm[nj_b2]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::b 
  if( perm[nj_b]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b] ];
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_b)->second ]);
  extend_PS( ps, PSPart::b, E, MB, dir, perm[nj_b], PSVar::cos_b, PSVar::phi_b, PSVar::E_b,  (perm[nj_b]>=0?TFType::bReco:TFType::bLost) ); 

  /////  PSPart::bbar  
  if( perm[nj_bbar]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_bbar] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_bbar)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_bbar)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E    = hypo==Hypothesis::TTBB ? 
    E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_bbar)->second ]) : 
    solve( ps.lv(PSPart::b), DMH2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::bbar, E, MB, dir, perm[nj_bbar], PSVar::cos_bbar, PSVar::phi_bbar, PSVar::E_bbar,  (perm[nj_bbar]>=0?TFType::bReco:TFType::bLost) ); 

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_LH(): END" << endl;
  }
#endif

  return accept;
}

int MEM::Integrand::create_PS_HH(MEM::PS& ps, const double* x, const vector<int>& perm ) const { 
  
#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_HH(): START" << endl;
  }
#endif

  // corrupted phase space
  int accept{0};

  // store temporary values to build four-vectors
  double E     {0.};
  double E_LOW {0.};
  double E_HIGH{0.};
  double E_REC {numeric_limits<double>::max()};
  TVector3 dir (1.,0.,0.);

  // map a quark to an index inside the obs_ collections
  size_t nj_q1    =  map_to_part.find(PSPart::q1)->second; 
  size_t nj_qbar1 =  map_to_part.find(PSPart::qbar1)->second; 
  size_t nj_b1    =  map_to_part.find(PSPart::b1)->second; 
  size_t nj_q2    =  map_to_part.find(PSPart::q2)->second; 
  size_t nj_qbar2 =  map_to_part.find(PSPart::qbar2)->second; 
  size_t nj_b2    =  map_to_part.find(PSPart::b2)->second; 
  size_t nj_b     =  map_to_part.find(PSPart::b)->second;
  size_t nj_bbar  =  map_to_part.find(PSPart::bbar)->second;

  /////  PSPart::q1
  if( perm[ nj_q1 ]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_q1] ]; 
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_Q) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_Q);
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_q1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_q1)->second ] );
    E_LOW   = MQ;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_q1)->second ]);
  extend_PS( ps, PSPart::q1, E, MQ, dir, perm[nj_q1], PSVar::cos_q1, PSVar::phi_q1, PSVar::E_q1, (perm[nj_q1]>=0?TFType::qReco:TFType::qLost) ); 

  /////  PSPart::qbar1
  if( perm[nj_qbar1]>=0 ){
    dir     = obs_jets[ perm[nj_qbar1] ]->p4().Vect().Unit();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_qbar1)->second ] );
  }
  E    = solve( ps.lv(PSPart::q1), DMW2 , MQ, dir, E_REC, accept );
  extend_PS( ps, PSPart::qbar1, E, MQ, dir, perm[nj_qbar1], PSVar::cos_qbar1, PSVar::phi_qbar1, PSVar::E_qbar1,  (perm[nj_qbar1]>=0?TFType::qReco:TFType::qLost) ); 

  /////  PSPart::b1
  if( perm[nj_b1]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b1] ]; 
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b1)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b1)->second ] );
  }
  E       = solve(ps.lv(PSPart::q1) + ps.lv(PSPart::qbar1), DMT2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::b1, E, MB , dir, perm[nj_b1], PSVar::cos_b1, PSVar::phi_b1, PSVar::E_b1,  (perm[nj_b1]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::q2
  if( perm[ nj_q2 ]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_q2] ]; 
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_Q) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_Q);
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_q2)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_q2)->second ] );
    E_LOW   = MQ;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_q2)->second ]);
  extend_PS( ps, PSPart::q2, E, MQ, dir, perm[nj_q2], PSVar::cos_q2, PSVar::phi_q2, PSVar::E_q2, (perm[nj_q2]>=0?TFType::qReco:TFType::qLost) ); 

  /////  PSPart::qbar2
  if( perm[nj_qbar2]>=0 ){
    dir = obs_jets[ perm[nj_qbar2] ]->p4().Vect().Unit();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_qbar2)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_qbar2)->second ] );
  }
  E    = solve( ps.lv(PSPart::q2), DMW2 , MQ, dir, E_REC, accept );
  extend_PS( ps, PSPart::qbar2, E, MQ, dir, perm[nj_qbar2], PSVar::cos_qbar2, PSVar::phi_qbar2, PSVar::E_qbar2,  (perm[nj_qbar2]>=0?TFType::qReco:TFType::qLost) ); 
  
  /////  PSPart::b2
  if( perm[nj_b2]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b2] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b2)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b2)->second ] );
  }
  E       = solve( ps.lv(PSPart::q2) + ps.lv(PSPart::qbar2), DMT2, MB, dir, E_REC, accept );
  extend_PS( ps, PSPart::b2, E, MB, dir, perm[nj_b2], PSVar::cos_b2, PSVar::phi_b2, PSVar::E_b2,  (perm[nj_b2]>=0?TFType::bReco:TFType::bLost) ); 
  
  /////  PSPart::b
  if( perm[nj_b]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_b] ];
    dir     = obj->p4().Vect().Unit();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);    
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_b)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_b)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E       = E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_b)->second ]);
  extend_PS( ps, PSPart::b, E, MB, dir, perm[nj_b], PSVar::cos_b, PSVar::phi_b, PSVar::E_b,  (perm[nj_b]>=0?TFType::bReco:TFType::bLost) ); 

  /////  PSPart::bbar   
  if( perm[nj_bbar]>=0 ){
    MEM::Object* obj = obs_jets[ perm[nj_bbar] ];
    dir     = obj->p4().Vect().Unit();
    E_REC   = obj->p4().E();
    E_LOW   = obj->getObs(Observable::E_LOW_B) ;
    E_HIGH  = obj->getObs(Observable::E_HIGH_B);        
  }
  else{
    dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_bbar)->second ]) );
    dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_bbar)->second ] );
    E_LOW   = MB;
    E_HIGH  = cfg.emax;
  }
  E    = hypo==Hypothesis::TTBB ?  
    E_LOW + (E_HIGH-E_LOW)*(x[ map_to_var.find(PSVar::E_bbar)->second ]) : 
    solve( ps.lv(PSPart::b), DMH2, MB, dir, E_REC, accept);
  extend_PS( ps, PSPart::bbar, E, MB, dir, perm[nj_bbar], PSVar::cos_bbar, PSVar::phi_bbar, PSVar::E_bbar,  (perm[nj_bbar]>=0?TFType::bReco:TFType::bLost) ); 

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_HH(): END" << endl;
  }
#endif

  return accept;
}


int MEM::Integrand::create_PS_TTH(MEM::PS& ps, const double* x, const vector<int>& perm ) const {

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_TTH(): START" << endl;
  }
#endif

  // corrupted phase space
  int accept{0};

  // store temporary values to build four-vectors
  double P{0.};
  TVector3 dir(1.,0.,0.);

  dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_t)->second ]) );
  dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_t)->second ] );
  P = x[ map_to_var.find(PSVar::P_t)->second ];
  extend_PS_nodebug( ps, PSPart::t, sqrt(P*P+MTOP2), MTOP, dir);

  dir.SetTheta( TMath::ACos( x[ map_to_var.find(PSVar::cos_tbar)->second ]) );
  dir.SetPhi  ( x[ map_to_var.find(PSVar::phi_tbar)->second ] );
  P = x[ map_to_var.find(PSVar::P_tbar)->second ];
  extend_PS_nodebug( ps, PSPart::tbar, sqrt(P*P+MTOP2), MTOP, dir);

  double Px = -ps.lv(PSPart::t).Px()-ps.lv(PSPart::tbar).Px();
  double Py = -ps.lv(PSPart::t).Py()-ps.lv(PSPart::tbar).Py();
  dir = TVector3(Px,Py, x[ map_to_var.find(PSVar::Pz_h)->second] );
  extend_PS_nodebug( ps, PSPart::h, sqrt(dir.Mag2()+MH2), MH, dir.Unit());


#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::create_PS_LH(): END" << endl;
  }
#endif

  return accept;
}



double MEM::Integrand::probability(const double* x, const std::size_t& n_perm ) const {

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::probability(): START" << endl;
  }
#endif

  // the total probability
  double p{1.0};
  if( !cfg.int_code ) return p;

  // create phas-space point and test if it is physical
  PS ps(ps_dim);
  int accept = create_PS(ps, x, perm_indexes_assumption[n_perm]);

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ) ps.print(cout);
#endif
  
  if( accept<0 ){
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration){
      cout << "\tCORRUPTED PS (no solution): return 0." << endl;      
    }
#endif
    ++(const_cast<Integrand*>(this)->n_skip);
    return 0.;
  }

  p *= constants();
  p *= transfer(ps, perm_indexes_assumption[n_perm], accept);
  if(cfg.tf_suppress && accept>=cfg.tf_suppress){
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\tTransfer functions out-of-range " << accept << " times: return from this PS before calculatin matrix()" << endl;
    }
#endif
    return 0.;
  }
  p *= matrix(ps);


#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::probability(): END" << endl;
  }
#endif

  return p;
}

double MEM::Integrand::matrix(const PS& ps) const {

  if( fs==FinalState::TTH ){ return matrix_nodecay(ps); }

  double m{1.};

  double x1  {0.}; // x1 fraction
  double x2  {0.}; // x2 fraction

  LV lv_q1    = ps.lv(PSPart::q1);
  LV lv_qbar1 = ps.lv(PSPart::qbar1);
  LV lv_b1    = ps.lv(PSPart::b1);
  LV lv_q2    = ps.lv(PSPart::q2);
  LV lv_qbar2 = ps.lv(PSPart::qbar2);
  LV lv_b2    = ps.lv(PSPart::b2);
  LV lv_b     = ps.lv(PSPart::b);
  LV lv_bbar  = ps.lv(PSPart::bbar);

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\t\tFilling m..." << endl;
    cout << "\t\tCheck masses: m(W1)=" << (lv_q1+lv_qbar1).M() << ", m(t1)=" << (lv_q1+lv_qbar1+lv_b1).M()
	 << ", m(W2)=" << (lv_q2+lv_qbar2).M() << ", m(t2)=" << (lv_q2+lv_qbar2+lv_b2).M() 
	 << ", m(H)=" << (lv_b+lv_bbar).M() << endl;
  }
#endif

  m *= t_decay_amplitude(lv_q1, lv_qbar1, lv_b1, ps.charge(PSPart::q1) );
  m *= t_decay_amplitude(lv_q2, lv_qbar2, lv_b2, ps.charge(PSPart::q2) );
  m *= H_decay_amplitude(lv_b, lv_bbar);
  m *= scattering( lv_q1+lv_qbar1+lv_b1, lv_q2+lv_qbar2+lv_b2, lv_b, lv_bbar, x1, x2);
  m *= pdf( x1, x2 , lv_b.Pt()+lv_bbar.Pt() );

  if( TMath::IsNaN(m) ){
    cout << "\tA NaN occurred while evaluation m..." << endl;
    m = 0.;
    return m;
  }

  return m;
}

double MEM::Integrand::matrix_nodecay(const PS& ps) const {
  double m{1.};

  double x1  {0.}; // x1 fraction
  double x2  {0.}; // x2 fraction

  LV lv_t     = ps.lv(PSPart::t);
  LV lv_tbar  = ps.lv(PSPart::tbar);
  LV lv_b     = fs==FinalState::TTH ? ps.lv(PSPart::h)      : ps.lv(PSPart::b);
  LV lv_bbar  = fs==FinalState::TTH ? LV(1e-06,0.,0.,1e-06) : ps.lv(PSPart::bbar);
  LV lv_h     = lv_b+lv_bbar;

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\t\tFilling m..." << endl;
    cout << "\t\tCheck masses: m(t1)=" << lv_t.M() << ", m(t2)=" << lv_tbar.M() << ", m(h)= " << (lv_b+lv_bbar).M() << endl;
  }
#endif

  m *= scattering( lv_t, lv_tbar, lv_b, lv_bbar, x1, x2);
  m *= pdf( x1, x2 , lv_h.Pt() );

  // check this: it won't work for tt+bb
  double Jac = lv_t.Beta()*lv_t.Vect().Mag()/2 * lv_tbar.Beta()*lv_tbar.Vect().Mag()/2 * (1./lv_h.E()/2);
  m *= Jac;

  if( TMath::IsNaN(m) ){
    cout << "\tA NaN occurred while evaluation m..." << endl;
    m = 0.;
    return m;
  }

  return m;
}

double MEM::Integrand::transfer(const PS& ps, const vector<int>& perm, int& accept) const {

  double w{1.};
  if( !(cfg.int_code&IntegrandType::Transfer) ) return w;
  if( fs==FinalState::TTH ) return w;

  double nu_x {0.}; // total nu's px
  double nu_y {0.}; // total nu's py
  double corr_nu_x {0.}; // sum of dPx
  double corr_nu_y {0.}; // sum of dPy
  double rho_x{0.}; // recoil px
  double rho_y{0.}; // recoil py
  double pT_x {0.}; // pT px
  double pT_y {0.}; // pT py

  // subtract MET from the recoil
  rho_x -= obs_mets[0]->p4().Px();
  rho_x -= obs_mets[0]->p4().Py();

  // Dealing with jets and leptons
  PSMap::const_iterator p;
  for( p = ps.begin() ; p != ps.end() ; ++p ){    

    if( isLepton  ( p->second.type ) ){
      MEM::Object* obj = obs_leptons[ map_to_part.find(p->first)->second ];
      // subtract from recoil
      rho_x -= obj->p4().Px();
      rho_y -= obj->p4().Py();

      // subtract from total pT
      pT_x  -= p->second.lv.Px();
      pT_y  -= p->second.lv.Py();
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\tDealing with a lepton..." << endl;
	cout << "\t\trho_x -= " << obj->p4().Px() << ", pT_x -= " << p->second.lv.Px() << endl;
	cout << "\t\trho_y -= " << obj->p4().Py() << ", pT_y -= " << p->second.lv.Py() << endl;
      }
#endif
      continue;
    }
    if( isNeutrino( p->second.type ) ){
      // add up to neutrino
      nu_x += p->second.lv.Px();
      nu_y += p->second.lv.Py();

      // subtract from total pT
      pT_x -= p->second.lv.Px();
      pT_y -= p->second.lv.Py();
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\tDealing with a neutrino..." << endl;
	cout << "\t\tnu_x -= " <<  p->second.lv.Px() << ", pT_x -= " << p->second.lv.Px() << endl;
        cout << "\t\tnu_y -= " <<  p->second.lv.Py() << ", pT_y -= " << p->second.lv.Py() << endl;
      }
#endif
      continue;
    }      

    // observables and generated-level quantities
    // if the parton is matched, test value of jet energy

    // subtract from total pT
    pT_x  -= p->second.lv.Px();
    pT_y  -= p->second.lv.Py();    

    double e_gen  {p->second.lv.E()}; 
    double eta_gen{p->second.lv.Eta()};     
    int jet_indx  = perm[ map_to_part.find(p->first)->second ];
    double e_rec{0.};
    if( jet_indx>=0 ){
      MEM::Object* obj = obs_jets[ jet_indx ];
      e_rec =  obj->p4().E();
      rho_x -= obj->p4().Px();
      rho_y -= obj->p4().Py();
      corr_nu_x += (e_rec-e_gen)*obj->p4().Px()/obj->p4().Pt();
      corr_nu_y += (e_rec-e_gen)*obj->p4().Py()/obj->p4().Pt();
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\tDealing with a jet..." << endl;
	cout << "\t\trho_x -= " << obj->p4().Px() << ", pT_x -= " << p->second.lv.Px() << endl;
	cout << "\t\trho_y -= " << obj->p4().Py() << ", pT_y -= " << p->second.lv.Py() << endl;
	cout << "\t\tdE_x   = " << (e_rec-e_gen)*obj->p4().Px()/obj->p4().P() << endl;
	cout << "\t\tdE_y   = " << (e_rec-e_gen)*obj->p4().Py()/obj->p4().P() << endl;
      }
#endif
    }
    else{
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\tDealing with a missed jet..." << endl;
	cout << "\t\trho_x -= " << 0 << ", pT_x -= " << p->second.lv.Px() << endl;
	cout << "\t\trho_y -= " << 0 << ", pT_y -= " << p->second.lv.Py() << endl;
      }
#endif
    }

    // build x,y vectors 
    double y[1] = { e_rec };
    double x[2] = { e_gen, eta_gen };
    w *= transfer_function( y, x, p->second.type, accept, cfg.tf_offscale, debug_code ); 
  }

  // Dealing with the MET
  double y_MET[2] = { obs_mets[0]->p4().Px(), obs_mets[0]->p4().Py() };
  double x_Nu [2] = { nu_x-corr_nu_x, nu_y-corr_nu_y };
  if( !(cfg.int_code&IntegrandType::Recoil) ){
    x_Nu[0] += corr_nu_x;
    x_Nu[1] += corr_nu_y;
  }
  w *= transfer_function( y_MET, x_Nu, TFType::MET, accept, cfg.tf_offscale, debug_code );

  // Dealing with the recoil
  double y_rho[1] = { (extra_jets>0 ? TF_RECOIL_param[2]+1. : sqrt(rho_x*rho_x + rho_y*rho_y)) };
  double x_pT [1] = { sqrt(pT_x*pT_x + pT_y*pT_y)  };
  if( cfg.int_code&IntegrandType::Sudakov ) 
    w *= transfer_function( y_rho, x_pT, TFType::Recoil, accept, cfg.tf_offscale, debug_code );

  if( TMath::IsNaN(w) ){
    cout << "\tA NaN occurred while evaluation w..." << endl;
    w = 0.;
    const_cast<Integrand*>(this)->error_code = 1;
    return w;
  }

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration){
    cout << "\tTotal transfer function: " << w 
	 << " (" << accept << " functions are out-of-range by more than " << cfg.tf_offscale << " sigmas" << endl;
  }
#endif

  return w;
}


double MEM::Integrand::scattering(const TLorentzVector& top, const TLorentzVector& atop, const TLorentzVector& b1, const TLorentzVector& b2,
				  double& x1, double& x2) const{

  // return value (address passed to OpenLoops)
  double M2{1.};  

  // temporary objects
  TLorentzVector t, tx, b, bx, h, sum;
  t.SetPtEtaPhiM ( top.Pt(),     top.Eta(),     top.Phi(),    MTOP);
  tx.SetPtEtaPhiM( atop.Pt(),    atop.Eta(),    atop.Phi(),   MTOP);
  b.SetPtEtaPhiM ( b1.Pt(),      b1.Eta(),      b1.Phi(),       0.);
  bx.SetPtEtaPhiM( b2.Pt(),      b2.Eta(),      b2.Phi(),       0.);
  h.SetPtEtaPhiM ( (b1+b2).Pt(), (b1+b2).Eta(), (b1+b2).Phi(),  MH);

  // the total sum (needed to get the boost factor);
  TLorentzVector vSum = hypo==Hypothesis::TTH ? t+tx+h : t+tx+b+bx;

  if(vSum.E()>cfg.sqrts){
    x1 = .99;
    x2 = .99;
    return 0.;
  }

  // boost such that SumPx = SumPy = 0
  TVector3 boostPt( vSum.Px()/vSum.E(), vSum.Py()/vSum.E(), 0.0 );
  bool apply_boost{vSum.Px()>1. || vSum.Py()>1.};

  if( apply_boost ){ // some tolerance!
    t.Boost  ( -boostPt );
    tx.Boost ( -boostPt );
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration && apply_boost){
      cout << "\t\tBoost system along the (x,y) plane: beta = (" 
	   << boostPt.Px() << ", " << boostPt.Py() << ", " << boostPt.Pz() << ")" << endl;
    }
#endif
  }

  if(hypo==Hypothesis::TTH){
    h.Boost  ( -boostPt );
    // fix for rounding
    double hPx = -(t.Px() + tx.Px());
    double hPy = -(t.Py() + tx.Py());
    double hPz = h.Pz();
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration && apply_boost){
      cout << "\t\tRounding Higgs px by " << (hPx-h.Px())/h.Px()*100 << "%" << endl;
      cout << "\t\tRounding Higgs py by " << (hPy-h.Py())/h.Py()*100 << "%" << endl;
    }
#endif
    h.SetPxPyPzE( hPx, hPy, hPz, sqrt(hPx*hPx + hPy*hPy + hPz*hPz + MH*MH) );    
    sum = t+tx+h;
  }
  else{
    b.Boost  ( -boostPt );
    bx.Boost ( -boostPt );
    // fix for rounding
    double bPx = -(t.Px() + tx.Px() + bx.Px());
    double bPy = -(t.Py() + tx.Py() + bx.Py());
    double bPz = b.Pz();
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration && apply_boost){
      cout << "\t\tRounding b px by " << (bPx-b.Px())/b.Px()*100 << "%" << endl;
      cout << "\t\tRounding b py by " << (bPy-b.Py())/b.Py()*100 << "%" << endl;
    }
#endif
    b.SetPxPyPzE( bPx, bPy, bPz, sqrt(bPx*bPx + bPy*bPy + bPz*bPz ) );
    sum =  t+tx+b+bx;
  }
  
  // update x1 and x2
  double E  = sum.E();
  double Pz = sum.Pz();
  x1 = ( Pz + E)/cfg.sqrts;
  x2 = (-Pz + E)/cfg.sqrts;
  if( !(cfg.int_code&IntegrandType::ScattAmpl) ) return M2;

  // create gluon p4s
  TLorentzVector g1 = TLorentzVector(0.,0.,  (E+Pz)/2., (E+Pz)/2.);
  TLorentzVector g2 = TLorentzVector(0.,0., -(E-Pz)/2., (E-Pz)/2.);

  // needed to interface with OpenLoops
  double ccP_0[20] = {
    g1.E(), g1.Px(), g1.Py(), g1.Pz(),
    g2.E(), g2.Px(), g2.Py(), g2.Pz(),
    h.E(),  h.Px(),  h.Py(),  h.Pz(),
    t.E(),  t.Px(),  t.Py(),  t.Pz(),
    tx.E(), tx.Px(), tx.Py(), tx.Pz()
  };

  double ccP_1[24] = {
    g1.E(), g1.Px(), g1.Py(), g1.Pz(),
    g2.E(), g2.Px(), g2.Py(), g2.Pz(),
    t.E(),  t.Px(),  t.Py(),  t.Pz(),
    tx.E(), tx.Px(), tx.Py(), tx.Pz(),
    b.E(),  b.Px(),  b.Py(),  b.Pz(),
    bx.E(), bx.Px(), bx.Py(), bx.Pz(),
  };

  // call OpenLoops functions
  switch(hypo){
  case Hypothesis::TTH:
    pphttxcallme2born_  (const_cast<double*>(&M2), ccP_0, const_cast<double*>(&MTOP), const_cast<double*>(&MH));
    break; 
  case Hypothesis::TTBB:
    ppttxbbxcallme2born_(const_cast<double*>(&M2), ccP_1, const_cast<double*>(&MTOP), const_cast<double*>(&MH));
    break;
  default:
    break;
  }

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\t\tTotal (px,py,pz,E) = (" <<  sum.Px() << "," <<  sum.Py()<< "," <<  sum.Pz()<< "," <<  sum.E() << ")" << endl;
    cout << "\t\tGluons (x1,x2)     = (" << x1 << "," << x2 << ")" << endl;
    cout << "\t\tM2 (OpenLoops)     = " << M2 << endl;
  }
#endif

  return M2;
}

double MEM::Integrand::pdf(const double& x1, const double& x2, const double& dynamical ) const{
  double p{1.};
  if( !(cfg.int_code&IntegrandType::PDF) ) return p;

  if(x1>0.99 || x2>0.99) return 0.;

  double Q{2*MTOP};
  switch( hypo ){
  case Hypothesis::TTH:
    Q = (2*MTOP + MH)/2;
    break;
  case Hypothesis::TTBB:
    Q = TMath::Sqrt( 4*MTOP*MTOP + TMath::Power(dynamical, 2) );
    break;
  default:
    break;
  }

  double f1 =  LHAPDF::xfx(1, x1, Q, 0)/x1;
  double f2 =  LHAPDF::xfx(1, x2, Q, 0)/x2;

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\t\tPDF(x1,Q)*PDF(x2,Q) = " << f1 << "*" << f2 << endl;
  }
#endif

  p = (f1*f2)/(x1*x2);

  return p;
}

double MEM::Integrand::constants() const {
  double p{1.};
  if( !(cfg.int_code&IntegrandType::Constant) ) return p;
  return TMath::Power((2.*PI), int(4-3*ps_dim))/(cfg.sqrts*cfg.sqrts*cfg.sqrts*cfg.sqrts);
}

double MEM::Integrand::t_decay_amplitude(const TLorentzVector& q, const TLorentzVector& qbar, const TLorentzVector& b, const int& charge_q) const{
  double p{1.};
  if( !(cfg.int_code&IntegrandType::DecayAmpl) ) return p;

  p *= BWTOP;

  TLorentzVector w = q+qbar;
  TLorentzVector t = w+b;
  double InvJac = TMath::Abs(2*MW2/qbar.E()*( w.E() - w.Vect().Dot( b.Vect().Unit() )/b.Beta() ));
  double Jac    = (1./InvJac) * q.Vect().Mag() * qbar.Vect().Mag() * b.Vect().Mag() / (2*2*2);
  if( cfg.int_code&IntegrandType::Jacobian ) 
    p *= Jac;

  double x_e1 = 2*(q*t)/MTOP2;
  double x_e2 = 2*(qbar*t)/MTOP2;

  // if the flavour is determined, use formula. Otherwise, take average.
  double m2   = charge_q!=0 ? x_e1*(1-MUB-x_e1) : 0.5*(x_e1*(1-MUB-x_e1) + x_e2*(1-MUB-x_e2));
  m2 *= (32*PI*MTOP4*GEWK4/(MW*GW));
  if(m2<0){
    cout << "\tIntegrand::t_decay_amplitude() returned negative |M2|..." << endl;
    return 0.;
  }
  p *= m2;

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::t_decay_amplitude():" << endl;
    cout << "\t\tBreit-Wigner top = " << BWTOP << " GeV^-2" << endl;
    cout << "\t\tJacobian (Eqbar,Eb) -> (m2_qq, m2_qqb) = " << Jac << " GeV" << endl;
    cout << "\t\t|M2|(t->bqq') = " << m2 << (charge_q==0?" (charge symmetrised)" : "")  << endl;
    cout << "\t\tTotal = " << p << " GeV^-1" << endl;
  }
#endif

  return p;
}

double MEM::Integrand::H_decay_amplitude(const TLorentzVector& b, const TLorentzVector& bbar) const{
  double p{1.};
  if( !(cfg.int_code&IntegrandType::DecayAmpl) ) return p;

  double InvJac{1.0};
  double m2{1.0};
  if( hypo==Hypothesis::TTH ){
    p *= BWH;
    InvJac = TMath::Abs(2*(b.E() - b.Vect().Dot( bbar.Vect().Unit() )/bbar.Beta()));
    m2 = 2*YB2*MH2*PSHBB;
  }
  double Jac    = (1./InvJac) * b.Vect().Mag() * bbar.Vect().Mag() / (2*2);
  if( cfg.int_code&IntegrandType::Jacobian ) 
    p *= Jac; 

  p *= m2;

#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::H_decay_amplitude():" << endl;
    if( hypo==Hypothesis::TTH ){
      cout << "\t\tBreit-Wigner Higgs = " << BWH << " GeV^-2" << endl;
      cout << "\t\tJacobian (Eb,Ebbar) -> (E_b, m2_bb) = " << Jac << " GeV" << endl;
      cout << "\t\t|M2|(t->bqq') = " << m2 << " GeV^2" << endl;
      cout << "\t\tTotal = " << p << " GeV" << endl;
    }
    else{
      cout << "\t\tJacobian = " << Jac << " GeV^2" << endl;
    }
  }
#endif

  return p;
}

double MEM::Integrand::solve(const LV& p4_w, const double& DM2, const double& M, const TVector3& e_b, const double& target, int& accept ) const {

  double a     = DM2/p4_w.E();
  double b     = TMath::Cos(p4_w.Angle(e_b));
  if( M<1e-03 ){
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\t\tUse masless formula: " << a << "/(1-" << b << ")=" << a/(1-b) << endl;  
    }
#endif
    if( b<1. ) return a/(1-b);
    else{
      accept = -1;
      return numeric_limits<double>::max();
    } 
  }
  
  // use adimensional 'a', account for velocity<1
  a /= M;
  b *= p4_w.Beta();
  double a2    = a*a;
  double b2    = b*b;

  // this is needed to test the solutions
  double discr = a2 + b2 - a2*b2 - 1; 

  // make sure there is >0 solutions
  if( (a2 + b2 - 1)<0. ){
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\t\t(a2 + b2 - 1)<0. return max()" << endl;
    }
#endif
    accept = -1;
    return numeric_limits<double>::max();
  }

  // the roots
  double g_p = (a + TMath::Abs(b)*sqrt(a2 + b2 - 1))/(1-b2);
  double g_m = (a - TMath::Abs(b)*sqrt(a2 + b2 - 1))/(1-b2);

  // make sure this is >1 ( g_m < g_p )
  if( g_p < 1.0 ){
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\t\tg_p=" << g_p << ": return max()" << endl;
    }
#endif
    accept = -1;
    return numeric_limits<double>::max();
  }

  // remove unphysical root
  if( g_m < 1.0) g_m = g_p;

  // test for the roots
  switch( b>0 ){
  case true :
    if( discr<0 ){
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\t\tb>0 AND discr<0: return root closest to target" << endl;
      }
#endif
      return ( TMath::Abs(target-g_p)<TMath::Abs(target-g_m) ? g_p*M : g_m*M );
    }
#ifdef DEBUG_MODE
    if( debug_code&DebugVerbosity::integration ){
      cout << "\t\tb>0 AND discr>0: return g_p*M" << endl;
    }
#endif
    return g_p*M;
    break;
  case false:
    if( discr>0 ){
#ifdef DEBUG_MODE
      if( debug_code&DebugVerbosity::integration ){
	cout << "\t\tb<0 AND discr>0: return g_m*M" << endl;
      }
#endif
      return g_m*M;
    }
    break;
  }
#ifdef DEBUG_MODE
  if( debug_code&DebugVerbosity::integration ){
    cout << "\tIntegrand::solve(): END" << endl;
  }
#endif

  accept = -1;
  return numeric_limits<double>::max();
}
